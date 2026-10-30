/*
 * Hard loudness cap for the speaker of the TicWatch Pro 3 Ultra (rover).
 *
 * The speaker is driven by an Awinic AW882xx smart amplifier (chip ID
 * 0x1852) at address 0x34 on I2C bus 2 (/soc/i2c@78b6000/aw882xx_smartpa@34
 * in rover-audio.dtsi). Its driver is Mobvoi's closed audio_AW882xx.ko from
 * the vendor partition, which sets the amplifier's digital volume to 0 dB,
 * its maximum, at every stream start. Root can also reach the chip directly
 * through /dev/i2c-2 or the driver's own sysfs and debug files.
 *
 * All of those end in __i2c_transfer() (the vendor driver uses
 * i2c_master_send() and i2c_master_recv(); SMBus calls are emulated with
 * plain transfers on this bus, see i2c_spkcap_is_amp()), which calls
 * i2c_spkcap_begin() before handing the messages to the bus driver. It keeps
 * the amplifier at least SPKCAP_MIN_ATTEN below its full gain:
 *
 *  - a write to HAGCCFG4 (0x0c) that would set the volume (VOL, bits 15:8)
 *    to less attenuation than the cap goes out with the cap instead;
 *  - a write to SYSCTRL (0x04) that would raise the speaker gain (SPK_GAIN,
 *    bits 14:12) above its factory setting goes out with the factory one;
 *  - before any write to SYSCTRL goes out, the volume is read back and set
 *    to the cap if it is louder. The volume comes out of every reset (the
 *    driver's reset GPIO, a soft reset, power-up) at 0 dB, but the amplifier
 *    also comes out of it powered down, and only a write to SYSCTRL powers
 *    it up again. So the amplifier never runs above the cap, whatever wrote
 *    to it and in whatever order;
 *  - transfers to the amplifier that this check could not follow are refused
 *    with -EPERM: more than one register write in one transfer (a reset could
 *    sit between the volume check and the power-up), a write of more than one
 *    register in one message (whether the chip auto-increments is not
 *    documented), and I2C_M_NOSTART or I2C_M_REV_DIR_ADDR, with which a
 *    message's bytes no longer start with the register address. The vendor
 *    driver never sends any of these.
 *
 * The caller's buffers are never modified (they may be const): a clamped
 * write goes out from a copy, and i2c_spkcap_end() puts the caller's buffer
 * pointer back after the transfer.
 *
 * Not touched, on purpose: the boost converter's output voltage (it sets the
 * supply rail, so it only limits the swing; the gain of the class-D stage is
 * SPK_GAIN, and with the cap the output stays far below the rail), the
 * chip's hardware AGC (it can only lower the gain, is off in the factory
 * settings, and its thresholds are undocumented), and the I2S input level
 * and channel selection (they can only attenuate or select).
 *
 * The cap is calibrated against Mobvoi's calibrated playback path, the
 * vendor audio HAL: by ear, 0.1 s 1 kHz tones through it reach the user's
 * limit at -20 dBFS. The amplifier measures its own output voltage (its
 * V-sense, read back over I2S) and, at 0 dB volume, gave a 1 kHz amplitude
 * of 857 counts for -30 dBFS through the HAL and 3050 counts for -20 dBFS
 * through plain ALSA. Scaled linearly, -20 dBFS through the HAL is 2711
 * counts and a full-scale signal about 30500 counts: 21.0 dB apart, or
 * 21.7 dB if the HAL path compresses a little above -30 dBFS, as its
 * -40 dBFS data hint. A -40 dBFS blip through the HAL with a 22.0 dB cap
 * then measured 0.8 to 1.5 dB quieter still than that predicts. With
 * 22.0 dB the user heard full scale, through the HAL and through plain
 * ALSA, as "still not too loud", and chose 20.0 dB: full scale on any path
 * now lands within about a dB of -20 dBFS through the HAL, the user's
 * limit.
 *
 * /sys/kernel/speaker_cap/ shows the cap and how many writes were clamped
 * or refused since boot.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#define pr_fmt(fmt) "i2c-speaker-cap: " fmt

#include <linux/atomic.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "i2c-speaker-cap.h"

/* Where the amplifier is (rover-audio.dtsi). */
#define SPKCAP_BUS		2
#define SPKCAP_ADDR		0x34

/*
 * The cap: the least attenuation of the amplifier's digital volume, in the
 * chip's 0.5 dB steps. 40 = 20.0 dB below full gain.
 */
#define SPKCAP_MIN_ATTEN	40

/* AW882xx (chip ID 0x1852) registers: 8-bit address, 16-bit big-endian. */
#define AW_REG_SYSCTRL		0x04
#define AW_REG_HAGCCFG4		0x0c
/* SYSCTRL: SPK_GAIN, bits 14:12 = bits 6:4 of the first data byte. */
#define AW_SPK_GAIN_SHIFT	4
#define AW_SPK_GAIN_MASK	(0x7 << AW_SPK_GAIN_SHIFT)
#define AW_SPK_GAIN_FACTORY	4	/* AV16; 5 is AV20 */

/*
 * HAGCCFG4: VOL, bits 15:8 = the first data byte. Attenuation is
 * 6 dB * VOL[7:4] + 0.5 dB * VOL[3:0]: 0x00 is 0 dB, 0xf0 the driver's mute.
 * Awinic's own driver only writes VOL[3:0] < 12; larger values are counted
 * as 0 here, the loud side of whatever the chip makes of them.
 */
static unsigned int aw_vol_atten(u8 vol)
{
	unsigned int sixes = vol >> 4, halves = vol & 0x0f;

	return sixes * 12 + (halves < 12 ? halves : 0);
}

#define SPKCAP_VOL	((SPKCAP_MIN_ATTEN / 12) << 4 | SPKCAP_MIN_ATTEN % 12)

static atomic_t spkcap_clamped = ATOMIC_INIT(0);
static atomic_t spkcap_refused = ATOMIC_INIT(0);

bool i2c_spkcap_is_amp(const struct i2c_adapter *adap, u16 addr,
		       unsigned short flags)
{
	return adap->nr == SPKCAP_BUS && addr == SPKCAP_ADDR &&
	       !(flags & I2C_M_TEN);
}

static int spkcap_refuse(const char *why)
{
	atomic_inc(&spkcap_refused);
	pr_warn_ratelimited("refused a transfer to the speaker amplifier: %s\n",
			    why);
	return -EPERM;
}

static void spkcap_note_clamp(u8 reg, const u8 *from, const u8 *to)
{
	atomic_inc(&spkcap_clamped);
	pr_notice_once("reg 0x%02x: 0x%02x.. -> 0x%02x.. (further clamps are only counted)\n",
		       reg, from[0], to[0]);
}

/* Clamp the data bytes (big-endian, the first one holds bits 15:8). */
static void spkcap_clamp(u8 reg, u8 *data, int len)
{
	u8 gain;

	if (len < 1)
		return;
	switch (reg) {
	case AW_REG_HAGCCFG4:
		if (aw_vol_atten(data[0]) < SPKCAP_MIN_ATTEN)
			data[0] = SPKCAP_VOL;
		break;
	case AW_REG_SYSCTRL:
		gain = (data[0] & AW_SPK_GAIN_MASK) >> AW_SPK_GAIN_SHIFT;
		if (gain > AW_SPK_GAIN_FACTORY)
			data[0] = (data[0] & ~AW_SPK_GAIN_MASK) |
				  AW_SPK_GAIN_FACTORY << AW_SPK_GAIN_SHIFT;
		break;
	}
}

/*
 * Read the volume back from the chip and set it to the cap if it is louder.
 * Called with the bus locked, before a write to SYSCTRL goes out. Reads the
 * way the vendor driver does: register pointer, then a separate 2-byte read.
 */
static int spkcap_check_volume(struct i2c_adapter *adap)
{
	u8 reg = AW_REG_HAGCCFG4, val[2], fix[3];
	struct i2c_msg ptr = { .addr = SPKCAP_ADDR, .len = 1, .buf = &reg };
	struct i2c_msg rd = {
		.addr = SPKCAP_ADDR, .flags = I2C_M_RD, .len = 2, .buf = val,
	};
	struct i2c_msg wr = { .addr = SPKCAP_ADDR, .len = 3, .buf = fix };

	if (adap->algo->master_xfer(adap, &ptr, 1) != 1 ||
	    adap->algo->master_xfer(adap, &rd, 1) != 1)
		return -EIO;
	if (aw_vol_atten(val[0]) >= SPKCAP_MIN_ATTEN)
		return 0;

	fix[0] = AW_REG_HAGCCFG4;
	fix[1] = SPKCAP_VOL;
	fix[2] = val[1];
	if (adap->algo->master_xfer(adap, &wr, 1) != 1)
		return -EIO;
	spkcap_note_clamp(AW_REG_HAGCCFG4, val, fix + 1);
	return 0;
}

/*
 * Called by __i2c_transfer() with the bus locked, before the transfer.
 * Returns 0 to let it go ahead, possibly with one message's buffer swapped
 * for a clamped copy in @ctx (i2c_spkcap_end() swaps it back), or -EPERM or
 * -EIO to refuse it.
 */
int i2c_spkcap_begin(struct i2c_adapter *adap, struct i2c_msg *msgs, int num,
		     struct i2c_spkcap_ctx *ctx)
{
	struct i2c_msg *wr = NULL;
	bool amp = false, mangled = false;
	u8 reg;
	int i, ret;

	ctx->msg = NULL;
	if (adap->nr != SPKCAP_BUS)
		return 0;

	for (i = 0; i < num; i++) {
		struct i2c_msg *m = &msgs[i];

		if (m->flags & (I2C_M_NOSTART | I2C_M_REV_DIR_ADDR))
			mangled = true;
		if (!i2c_spkcap_is_amp(adap, m->addr, m->flags))
			continue;
		amp = true;
		/* Reads and register-pointer writes change nothing. */
		if ((m->flags & I2C_M_RD) || m->len < 2)
			continue;
		if (m->len > 3)
			return spkcap_refuse("write of several registers");
		if (wr)
			return spkcap_refuse("several writes in a transfer");
		wr = m;
	}
	if (!amp)
		return 0;
	if (mangled)
		return spkcap_refuse("I2C_M_NOSTART or I2C_M_REV_DIR_ADDR");
	if (!wr)
		return 0;

	/*
	 * The registers end at 0x7c. Should the chip ignore bit 7 of the
	 * address, 0x84 and 0x8c are treated like 0x04 and 0x0c.
	 */
	reg = wr->buf[0] & 0x7f;
	if (reg == AW_REG_SYSCTRL) {
		ret = spkcap_check_volume(adap);
		if (ret)
			return ret;
	}

	memcpy(ctx->buf, wr->buf, wr->len);
	spkcap_clamp(reg, ctx->buf + 1, wr->len - 1);
	if (!memcmp(ctx->buf, wr->buf, wr->len))
		return 0;

	spkcap_note_clamp(reg, wr->buf + 1, ctx->buf + 1);
	ctx->msg = wr;
	ctx->orig = wr->buf;
	wr->buf = ctx->buf;
	return 0;
}

static ssize_t attenuation_db_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u.%u\n", SPKCAP_MIN_ATTEN / 2,
			 SPKCAP_MIN_ATTEN % 2 * 5);
}

static ssize_t clamped_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&spkcap_clamped));
}

static ssize_t refused_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&spkcap_refused));
}

static struct kobj_attribute attenuation_db_attr = __ATTR_RO(attenuation_db);
static struct kobj_attribute clamped_attr = __ATTR_RO(clamped);
static struct kobj_attribute refused_attr = __ATTR_RO(refused);

static struct attribute *spkcap_attrs[] = {
	&attenuation_db_attr.attr,
	&clamped_attr.attr,
	&refused_attr.attr,
	NULL,
};

static const struct attribute_group spkcap_group = {
	.attrs = spkcap_attrs,
};

static int __init i2c_spkcap_init(void)
{
	struct kobject *kobj;

	pr_info("speaker amplifier %d-%04x held at least %u.%u dB below full gain\n",
		SPKCAP_BUS, SPKCAP_ADDR, SPKCAP_MIN_ATTEN / 2,
		SPKCAP_MIN_ATTEN % 2 * 5);
	kobj = kobject_create_and_add("speaker_cap", kernel_kobj);
	if (!kobj)
		return -ENOMEM;
	return sysfs_create_group(kobj, &spkcap_group);
}
late_initcall(i2c_spkcap_init);
