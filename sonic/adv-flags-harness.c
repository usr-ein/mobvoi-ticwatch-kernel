/*
 * The bt-adv-flags-length fix, checked in userspace (P-049, 2026-11-17).
 *
 * With BR/EDR off (this watch is LE only), create_instance_adv_data() adds a
 * 3-byte Flags field to an advertising instance's data that has none, but
 * Add Advertising's check, tlv_data_is_valid(), reserves those 3 bytes only
 * for discoverable or managed-flags instances. So 29 to 31 bytes of flagless
 * data pass the check, and __hci_req_update_adv_data() then writes up to 3
 * bytes past its 31-byte stack buffer. On the watch, with
 * CONFIG_CC_STACKPROTECTOR_STRONG, that is most likely a panic: never try it
 * on a kernel without the fix.
 *
 * Here, the two functions' logic is copied from net/bluetooth/mgmt.c and
 * hci_request.c (4.9.232, asteroidos-2.1), before (OLD) and after (NEW) the
 * fix, onto the same stack layout, and run under AddressSanitizer:
 *
 *   clang -g -fsanitize=address -fno-omit-frame-pointer -o /tmp/advh sonic/adv-flags-harness.c
 *   /tmp/advh new     # every case: refused or fits, exit 0
 *   /tmp/advh old     # 29 flagless bytes: stack-buffer-overflow, ASan aborts
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;

#define HCI_MAX_AD_LENGTH 31
#define EIR_FLAGS 0x01
#define EIR_TX_POWER 0x0a
#define LE_AD_LIMITED 0x01
#define LE_AD_GENERAL 0x02
#define LE_AD_NO_BREDR 0x04
#define MGMT_ADV_FLAG_CONNECTABLE (1u << 0)
#define MGMT_ADV_FLAG_DISCOV (1u << 1)
#define MGMT_ADV_FLAG_LIMITED_DISCOV (1u << 2)
#define MGMT_ADV_FLAG_MANAGED_FLAGS (1u << 3)
#define MGMT_ADV_FLAG_TX_POWER (1u << 4)

/* The adapter as on the watch: LE only, not discoverable, a TX power known. */
static const bool bredr_enabled = false;
static const signed char adv_tx_power = 12;

struct adv_info { u32 flags; u8 adv_data_len; u8 adv_data[HCI_MAX_AD_LENGTH]; };
struct hci_cp_le_set_adv_data { u8 length; u8 data[HCI_MAX_AD_LENGTH]; } __attribute__((packed));

/* include/net/bluetooth/hci_core.h, as is */
static void *eir_get_data(u8 *eir, size_t eir_len, u8 type, size_t *data_len)
{
	size_t parsed = 0;

	if (eir_len < 2)
		return NULL;
	while (parsed < eir_len - 1) {
		u8 field_len = eir[0];

		if (field_len == 0)
			break;
		parsed += field_len + 1;
		if (parsed > eir_len)
			break;
		if (eir[1] != type) {
			eir += field_len + 1;
			continue;
		}
		if (field_len == 1)
			return NULL;
		if (data_len)
			*data_len = field_len - 1;
		return &eir[2];
	}
	return NULL;
}

static bool flags_managed(u32 f)
{
	return f & (MGMT_ADV_FLAG_DISCOV | MGMT_ADV_FLAG_LIMITED_DISCOV | MGMT_ADV_FLAG_MANAGED_FLAGS);
}

/* mgmt.c tlv_data_max_len(), advertising data only */
static u8 tlv_data_max_len(u32 adv_flags)
{
	u8 max_len = HCI_MAX_AD_LENGTH;

	if (flags_managed(adv_flags))
		max_len -= 3;
	if (adv_flags & MGMT_ADV_FLAG_TX_POWER)
		max_len -= 3;
	return max_len;
}

/* mgmt.c tlv_data_is_valid(), advertising data only; fixed adds the reservation */
static bool tlv_data_is_valid(bool fixed, u32 adv_flags, u8 *data, u8 len)
{
	int i, cur_len;
	u8 max_len = tlv_data_max_len(adv_flags);

	if (fixed && !flags_managed(adv_flags) && !bredr_enabled &&
	    !eir_get_data(data, len, EIR_FLAGS, NULL))
		max_len -= 3;
	if (len > max_len)
		return false;
	for (i = 0, cur_len = 0; i < len; i += (cur_len + 1)) {
		cur_len = data[i];
		if (data[i + 1] == EIR_FLAGS && flags_managed(adv_flags))
			return false;
		if (data[i + 1] == EIR_TX_POWER && (adv_flags & MGMT_ADV_FLAG_TX_POWER))
			return false;
		if (i + cur_len >= len)
			return false;
	}
	return true;
}

/* hci_request.c create_instance_adv_data(); fixed adds the two room checks */
static u8 create_instance_adv_data(bool fixed, struct adv_info *adv_instance, u8 *ptr)
{
	u8 ad_len = 0, flags = 0;
	u32 instance_flags = adv_instance->flags;

	if (eir_get_data(adv_instance->adv_data, adv_instance->adv_data_len, EIR_FLAGS, NULL))
		goto skip_flags;
	if (instance_flags & MGMT_ADV_FLAG_DISCOV)
		flags |= LE_AD_GENERAL;
	if (instance_flags & MGMT_ADV_FLAG_LIMITED_DISCOV)
		flags |= LE_AD_LIMITED;
	if (!bredr_enabled)
		flags |= LE_AD_NO_BREDR;
	if (flags || (instance_flags & MGMT_ADV_FLAG_MANAGED_FLAGS)) {
		if (flags && (!fixed || adv_instance->adv_data_len <= HCI_MAX_AD_LENGTH - 3)) {
			ptr[0] = 0x02;
			ptr[1] = EIR_FLAGS;
			ptr[2] = flags;
			ad_len += 3;
			ptr += 3;
		}
	}
skip_flags:
	memcpy(ptr, adv_instance->adv_data, adv_instance->adv_data_len);
	ad_len += adv_instance->adv_data_len;
	ptr += adv_instance->adv_data_len;
	if ((instance_flags & MGMT_ADV_FLAG_TX_POWER) &&
	    (!fixed || ad_len <= HCI_MAX_AD_LENGTH - 3)) {
		ptr[0] = 0x02;
		ptr[1] = EIR_TX_POWER;
		ptr[2] = (u8)adv_tx_power;
		ad_len += 3;
	}
	return ad_len;
}

/* hci_request.c __hci_req_update_adv_data(): the same stack buffer */
static u8 update_adv_data(bool fixed, struct adv_info *inst)
{
	struct hci_cp_le_set_adv_data cp;

	memset(&cp, 0, sizeof(cp));
	return create_instance_adv_data(fixed, inst, cp.data);
}

/* One manufacturer-data field of n bytes in all (n >= 4). */
static void mfr(u8 *d, u8 n)
{
	memset(d, 0x5a, n);
	d[0] = n - 1;
	d[1] = 0xff;
}

int main(int argc, char **argv)
{
	bool fixed = argc > 1 && strcmp(argv[1], "new") == 0;
	int bad = 0;
	struct { const char *what; u32 flags; u8 n; bool with_flags_field; } cases[] = {
		{ "flagless, 28 bytes", 0, 28, false },
		{ "flagless, 29 bytes", 0, 29, false },
		{ "flagless, 31 bytes (Find My's size)", 0, 31, false },
		{ "own Flags field, 31 bytes (ioslink's kind)", 0, 31, true },
		{ "managed flags, 28 bytes", MGMT_ADV_FLAG_MANAGED_FLAGS, 28, false },
		{ "TX power, 25 bytes", MGMT_ADV_FLAG_TX_POWER, 25, false },
	};

	printf("%s code, LE only\n", fixed ? "NEW" : "OLD");
	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		struct adv_info inst = { .flags = cases[c].flags, .adv_data_len = cases[c].n };

		if (cases[c].with_flags_field) {
			inst.adv_data[0] = 0x02;
			inst.adv_data[1] = EIR_FLAGS;
			inst.adv_data[2] = LE_AD_GENERAL | LE_AD_NO_BREDR;
			mfr(inst.adv_data + 3, cases[c].n - 3);
		} else {
			mfr(inst.adv_data, cases[c].n);
		}
		if (!tlv_data_is_valid(fixed, inst.flags, inst.adv_data, inst.adv_data_len)) {
			printf("  %-44s refused by Add Advertising\n", cases[c].what);
			continue;
		}
		u8 len = update_adv_data(fixed, &inst);
		printf("  %-44s accepted, %u bytes to the controller%s\n", cases[c].what, len,
		       len > HCI_MAX_AD_LENGTH ? "  <-- PAST THE BUFFER" : "");
		if (len > HCI_MAX_AD_LENGTH)
			bad = 1;
	}
	return bad;
}
