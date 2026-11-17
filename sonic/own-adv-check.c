/*
 * The bt-adv-own-address fix checked on the watch (P-049): an advertisement
 * from an address of its own, on the controller's vendor set (handle 1),
 * through mgmt as BlueZ's SonicAddress sends it. Root, detached, with btmon
 * capturing the vendor commands (0xfd54) and their answers:
 *
 *   docker run --rm -v "$T":/t sonic-asteroid-launcher-sdk:2.1 sh -c \
 *     '. /opt/asteroid-sdk/environment-setup-armv7vehf-neon-oe-linux-gnueabi; $CC -O2 -o /t/own-adv-check /t/sonic/own-adv-check.c'
 *     (with the SDK volume mounted as for bluetoothd: -v sonic-asteroid-sdk-2.1:/opt/asteroid-sdk:ro)
 *   scp "$T/own-adv-check" sonic-screwdriver:/tmp/
 *   ssh sonic-screwdriver 'systemd-run --unit=own-adv-btmon --collect timeout 60 btmon -w /tmp/own-adv.btsnoop;
 *     systemd-run --unit=own-adv-check --collect /tmp/own-adv-check "#27-1234567" 20'
 *   ssh sonic-screwdriver 'journalctl -u own-adv-check -o short-precise --no-pager;
 *     btmon -r /tmp/own-adv.btsnoop | grep -A4 "0x3f|0x0154"; rm -f /tmp/own-adv-check /tmp/own-adv.btsnoop'
 *
 * It refuses to run unless `uname -v` starts with the build tag given and a
 * space. The data is the SIG's test company 0xFFFF, never Apple's, and the
 * addresses are invented, so nothing is taken for a tracker. Instance 5, the
 * last, so that BlueZ's own (from 1) never meet it. Steps, each PASS or FAIL:
 *   1. Read Advertising Features lists the private flag, BIT(31).
 *   2. Refused: an address that isn't static random; a scan response; a
 *      second flag besides BIT(31) (Invalid Parameters).
 *   3. Added: instance 5, 31 bytes, C0:FF:EE:00:00:01; listed after.
 *   4. A standard instance can't take instance 5 now (Busy), nor can a
 *      second own-address instance, 4 (Busy).
 *   5. Held for the seconds given (the vendor set advertises meanwhile),
 *      then added again under 5 with C0:FF:EE:00:00:02: updated in place.
 *   6. Removed; gone from the list.
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#define AF_BT 31
#define BTPROTO_HCI 1
#define HCI_CHANNEL_CONTROL 3
#define HCI_DEV_NONE 0xffff
#define OP_READ_ADV_FEATURES 0x003d
#define OP_ADD_ADVERTISING 0x003e
#define OP_REMOVE_ADVERTISING 0x003f
#define EV_CMD_COMPLETE 0x0001
#define EV_CMD_STATUS 0x0002
#define FLAG_SONIC_OWN_ADDR (1u << 31)
#define INSTANCE 5

struct sockaddr_hci {
	unsigned short hci_family;
	unsigned short hci_dev;
	unsigned short hci_channel;
};

static int sk, fails;

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint16_t get16(const uint8_t *p) { return p[0] | p[1] << 8; }

static void check(int ok, const char *what)
{
	printf("own-adv-check: %s: %s\n", what, ok ? "PASS" : "FAIL");
	fflush(stdout);
	if (!ok)
		fails++;
}

/* Sends one command to hci0 and waits up to 5 s for its answer: the status,
 * and the return parameters in rp (rplen in, their length out).
 */
static int command(uint16_t op, const uint8_t *param, uint16_t len, uint8_t *rp, int *rplen)
{
	uint8_t buf[512];

	put16(buf, op);
	put16(buf + 2, 0);	/* hci0 */
	put16(buf + 4, len);
	memcpy(buf + 6, param, len);
	if (write(sk, buf, 6 + len) != 6 + len)
		return -1;
	for (;;) {
		struct pollfd pfd = { .fd = sk, .events = POLLIN };
		ssize_t n;

		if (poll(&pfd, 1, 5000) != 1)
			return -2;
		n = read(sk, buf, sizeof(buf));
		if (n < 9)
			continue;
		uint16_t ev = get16(buf), index = get16(buf + 2);
		if (index != 0 || get16(buf + 6) != op)
			continue;	/* another event, or another command's */
		if (ev == EV_CMD_STATUS)
			return buf[8];
		if (ev == EV_CMD_COMPLETE) {
			int got = n - 9;
			if (rp && rplen) {
				if (got > *rplen)
					got = *rplen;
				memcpy(rp, buf + 9, got);
				*rplen = got;
			}
			return buf[8];
		}
	}
}

/* Read Advertising Features: the supported flags, and whether the instance is listed. */
static int features(uint32_t *flags, int instance)
{
	uint8_t rp[64];
	int len = sizeof(rp), listed = 0, status;

	status = command(OP_READ_ADV_FEATURES, NULL, 0, rp, &len);
	if (status || len < 8)
		return -1;
	*flags = rp[0] | rp[1] << 8 | rp[2] << 16 | (uint32_t)rp[3] << 24;
	for (int i = 0; i < rp[7] && 8 + i < len; i++)
		if (rp[8 + i] == instance)
			listed = 1;
	return listed;
}

/* Add Advertising: instance, flags, data (n bytes of a 0xFFFF field), optional
 * scan response, and the address (display order, as written) after it.
 */
static int add(int instance, uint32_t flags, int n, int scan_rsp, const uint8_t addr_shown[6])
{
	uint8_t p[11 + 31 + 31 + 6];
	int len = 11;

	memset(p, 0, sizeof(p));
	p[0] = instance;
	put32(p + 1, flags);
	p[9] = n;
	p[10] = scan_rsp ? 3 : 0;
	memset(p + len, 0x5a, n);
	p[len] = n - 1;		/* one manufacturer field: length, 0xff, 0xffff, data */
	p[len + 1] = 0xff;
	p[len + 2] = 0xff;
	p[len + 3] = 0xff;
	len += n;
	if (scan_rsp) {
		p[len] = 2; p[len + 1] = 0x0a; p[len + 2] = 0;	/* a TX power field */
		len += 3;
	}
	if (addr_shown) {
		for (int i = 0; i < 6; i++)
			p[len + i] = addr_shown[5 - i];	/* bdaddr_t is least significant first */
		len += 6;
	}
	return command(OP_ADD_ADVERTISING, p, len, NULL, NULL);
}

static int removal(int instance)
{
	uint8_t p[1] = { instance };

	return command(OP_REMOVE_ADVERTISING, p, 1, NULL, NULL);
}

int main(int argc, char **argv)
{
	static const uint8_t a1[6] = { 0xc0, 0xff, 0xee, 0x00, 0x00, 0x01 };
	static const uint8_t a2[6] = { 0xc0, 0xff, 0xee, 0x00, 0x00, 0x02 };
	static const uint8_t nonstatic[6] = { 0x40, 0xff, 0xee, 0x00, 0x00, 0x03 };
	struct sockaddr_hci addr = { AF_BT, HCI_DEV_NONE, HCI_CHANNEL_CONTROL };
	struct utsname u;
	char want[64];
	uint32_t flags;
	int hold, listed;

	if (argc != 3)
		return 2;
	hold = atoi(argv[2]);
	snprintf(want, sizeof(want), "%s ", argv[1]);
	if (uname(&u) || strncmp(u.version, want, strlen(want))) {
		printf("own-adv-check: refusing: this kernel is %s, not %s\n", u.version, argv[1]);
		return 2;
	}
	printf("own-adv-check: kernel %s\n", u.version);
	sk = socket(AF_BT, SOCK_RAW, BTPROTO_HCI);
	if (sk < 0 || bind(sk, (struct sockaddr *)&addr, sizeof(addr))) {
		printf("own-adv-check: mgmt socket: %s\n", strerror(errno));
		return 1;
	}

	listed = features(&flags, INSTANCE);
	check(listed >= 0 && (flags & FLAG_SONIC_OWN_ADDR), "1. the private flag is listed");

	check(add(INSTANCE, FLAG_SONIC_OWN_ADDR, 31, 0, nonstatic) == 0x0d, "2. a non-static address: Invalid Parameters");
	check(add(INSTANCE, FLAG_SONIC_OWN_ADDR, 28, 1, a1) == 0x0d, "2. with a scan response: Invalid Parameters");
	check(add(INSTANCE, FLAG_SONIC_OWN_ADDR | 1, 31, 0, a1) == 0x0d, "2. connectable too: Invalid Parameters");

	check(add(INSTANCE, FLAG_SONIC_OWN_ADDR, 31, 0, a1) == 0, "3. added: 31 bytes from C0:FF:EE:00:00:01");
	check(features(&flags, INSTANCE) == 1, "3. listed");

	check(add(INSTANCE, 0, 20, 0, NULL) == 0x0a, "4. a standard instance under 5: Busy");
	check(add(4, FLAG_SONIC_OWN_ADDR, 31, 0, a2) == 0x0a, "4. a second own-address instance: Busy");

	printf("own-adv-check: holding %d s\n", hold);
	fflush(stdout);
	sleep(hold);
	check(add(INSTANCE, FLAG_SONIC_OWN_ADDR, 31, 0, a2) == 0, "5. updated in place: C0:FF:EE:00:00:02");
	sleep(2);

	check(removal(INSTANCE) == 0, "6. removed");
	check(features(&flags, INSTANCE) == 0, "6. gone from the list");

	printf("own-adv-check: RESULT: %s\n", fails ? "FAIL" : "PASS");
	return fails ? 1 : 0;
}
