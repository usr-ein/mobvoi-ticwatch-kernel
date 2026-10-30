/*
 * Hard loudness cap for the speaker of the TicWatch Pro 3 Ultra: the hooks
 * the I2C core calls. See i2c-speaker-cap.c.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#ifndef _I2C_SPEAKER_CAP_H
#define _I2C_SPEAKER_CAP_H

#include <linux/i2c.h>
#include <linux/types.h>

/* i2c-core.c calls into i2c-speaker-cap.c, which is not a module of its own. */
#if IS_MODULE(CONFIG_I2C)
#error "the speaker cap (i2c-speaker-cap.c) needs CONFIG_I2C=y"
#endif

struct i2c_spkcap_ctx {
	struct i2c_msg *msg;	/* the message sent from buf instead, or NULL */
	u8 *orig;		/* its own buffer */
	u8 buf[3];
};

bool i2c_spkcap_is_amp(const struct i2c_adapter *adap, u16 addr,
		       unsigned short flags);
int i2c_spkcap_begin(struct i2c_adapter *adap, struct i2c_msg *msgs, int num,
		     struct i2c_spkcap_ctx *ctx);

static inline void i2c_spkcap_end(struct i2c_spkcap_ctx *ctx)
{
	if (ctx->msg)
		ctx->msg->buf = ctx->orig;
}

#endif /* _I2C_SPEAKER_CAP_H */
