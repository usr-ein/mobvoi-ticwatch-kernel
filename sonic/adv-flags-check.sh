#!/bin/sh
# The bt-adv-flags-length fix's check on the watch (P-049). Root, detached:
#
#   scp sonic/adv-flags-check.sh sonic-screwdriver:/tmp/
#   ssh sonic-screwdriver 'systemd-run --unit=adv-flags-check --collect sh /tmp/adv-flags-check.sh "#26-fa379e1"'
#   ssh sonic-screwdriver 'journalctl -u adv-flags-check -o short-precise --no-pager; rm -f /tmp/adv-flags-check.sh'
#
# It refuses to run unless `uname -v` starts with the build given: on a kernel
# without the fix, 31 flagless bytes are written past a 31-byte stack buffer
# (sonic/adv-flags-harness.c), which with the stack protector is a panic.
#
# Through mgmt, with the SIG's test company 0xFFFF (never Apple's data):
#   - 31 bytes of flagless advertising data must be refused, Invalid Parameters
#     (0x0d): the kernel's own Flags field would not fit;
#   - 28 bytes must pass the check: "Instance added", or Failed (0x03) when the
#     data is already in place; then the instance is removed again.
# Instance 5, the last, so that BlueZ's own (from 1) never meet it. The
# iPhone's link must be up before and after.
set -u
want=${1:?usage: adv-flags-check.sh <uname -v prefix, e.g. "#26-fa379e1">}
case "$(uname -v)" in
"$want "*) ;; # the whole build tag: #26-fa379e1 must not match #26-fa379e10
*) echo "adv-flags-check: refusing: this kernel is $(uname -v), not $want"; exit 2 ;;
esac
echo "adv-flags-check 120000000000" >/sys/power/wake_lock
log() { echo "adv-flags-check: $*"; }
fail=0
links() { hcitool con | grep -c ' LE ' ; }

log "kernel $(uname -v)"
log "LE links before: $(links)"

hex() { printf '%s' "$1"; i=0; while [ $i -lt "$2" ]; do printf '5a'; i=$((i + 1)); done; }
d31=$(hex 1effffff 27)   # one manufacturer field, 31 bytes, no Flags
d28=$(hex 1bffffff 24)   # the same, 28 bytes

out=$(timeout 10 bluetoothctl mgmt.add-adv -d "$d31" 5 2>&1)
case "$out" in
*"0x0d"*) log "31 flagless bytes: refused, Invalid Parameters: PASS" ;;
*) log "31 flagless bytes: not refused ($out): FAIL"; fail=1
   timeout 10 bluetoothctl mgmt.rm-adv 5 >/dev/null 2>&1 ;;
esac

out=$(timeout 10 bluetoothctl mgmt.add-adv -d "$d28" 5 2>&1)
case "$out" in
*"Instance added: 5"*) log "28 flagless bytes: accepted: PASS" ;;
*"0x03"*) log "28 flagless bytes: past the length check, Failed (0x03): PASS" ;;
*) log "28 flagless bytes: $out: FAIL"; fail=1 ;;
esac
out=$(timeout 10 bluetoothctl mgmt.rm-adv 5 2>&1)
log "instance 5 removed: $(printf '%s' "$out" | tr '\n' ' ')"
log "advertising instances now: $(timeout 10 bluetoothctl mgmt.advinfo 2>&1 | grep -i 'instances list' | tr '\n' ' ')"

log "LE links after: $(links)"
[ "$fail" = 0 ] && log "RESULT: PASS" || log "RESULT: FAIL"
echo adv-flags-check >/sys/power/wake_unlock
exit "$fail"
