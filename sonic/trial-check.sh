#!/bin/bash
# Health check of the kernel running on the watch, over SSH. Read-only, except
# that --confirm creates /run/sonic-trial-ok so the dead-man's switch keeps a
# healthy trial kernel running. Runs on the Mac. See BUILD.md.
#
#   sonic/trial-check.sh [--confirm] [BUILD_DIR]
#
# With BUILD_DIR, also checks that the running kernel is that build (uname -v)
# and that /proc/config.gz equals BUILD_DIR/config.
# Exit status: 0 healthy, 1 a check failed, 2 the watch is not on a trial kernel
# (with --confirm).
#
# Prints no addresses, names or notification content: the output may be pasted
# into reports.
set -uo pipefail

HOST=${SONIC_HOST:-sonic-screwdriver}
confirm=0
[ "${1:-}" = --confirm ] && { confirm=1; shift; }
build=${1:-}

r=$(ssh -o ConnectTimeout=20 "$HOST" 'bash -s' <<'EOF'
kv() { printf '%s=%s\n' "$1" "$2"; }
kv trial "$(grep -c 'sonic\.trial=1' /proc/cmdline)"
kv uname_r "$(uname -r)"
kv uname_v "$(uname -v)"
kv uptime_s "$(cut -d' ' -f1 /proc/uptime)"
kv config_sha256 "$(zcat /proc/config.gz | sha256sum | cut -d' ' -f1)"
kv download_mode "$(cat /sys/module/msm_poweroff/parameters/download_mode)"
kv switch "$(systemctl is-active kernel-trial.service)"
kv trial_ok_flag "$([ -e /run/sonic-trial-ok ] && echo yes || echo no)"
kv wlan0 "$(cat /sys/class/net/wlan0/operstate 2>/dev/null || echo missing)"
kv wlan0_driver "$(basename "$(readlink -f /sys/class/net/wlan0/device/driver)" 2>/dev/null)"
kv wlan0_ipv4 "$(ip -4 -o addr show dev wlan0 2>/dev/null | grep -c ' inet ')"
kv connman "$(connmanctl state 2>/dev/null | sed -n 's/^ *State = //p')"
kv hci0 "$([ -e /sys/class/bluetooth/hci0 ] && echo present || echo missing)"
kv bt_powered "$(timeout 10 bluetoothctl show 2>/dev/null | sed -n 's/^\s*Powered: //p' | head -n 1)"
kv bluetoothd "$(systemctl is-active bluetooth.service)"
kv ioslink "$(systemctl --user -M ceres@ is-active ioslink.service 2>/dev/null)"
kv ioslink_connects "$(journalctl -t ioslink -b 0 -o cat --no-pager | grep -c -E '^bluez: .* connected')"
kv ioslink_ancs_subscribed "$(journalctl -t ioslink -b 0 -o cat --no-pager | grep -c '^ancs: subscribed')"
kv launcher "$(systemctl --user -M ceres@ is-active asteroid-launcher.service 2>/dev/null)"
kv fb0 "$(cat /sys/class/graphics/fb0/name 2>/dev/null || echo missing)"
kv mdss_fb_bound "$(ls /sys/bus/platform/drivers/mdss_fb 2>/dev/null | grep -c 'mdss_fb_primary')"
kv dsi_bound "$(ls /sys/bus/platform/drivers/mdss_dsi_ctrl 2>/dev/null | grep -c 'mdss_dsi_ctrl0')"
kv backlight "$(cat /sys/class/leds/lcd-backlight/brightness 2>/dev/null)"
kv touch_input "$(grep -c 'Name="bt541_ts"' /proc/bus/input/devices)"
kv touch_bound "$([ -e /sys/bus/i2c/drivers/bt541_ts_device/4-0020 ] && echo yes || echo no)"
kv touch_irqs "$(awk '/bt541/ {s=0; for (i = 2; i <= NF; i++) if ($i ~ /^[0-9]+$/) s += $i; else break; print s}' /proc/interrupts)"
kv failed_units "$(systemctl --failed --no-legend --plain 2>/dev/null | awk '{print $1}' | tr '\n' ' ')"
kv klog_err "$(journalctl -k -b 0 -p err --no-pager -o cat | wc -l)"
kv klog_warn "$(journalctl -k -b 0 -p warning --no-pager -o cat | wc -l)"
kv klog_oops "$(journalctl -k -b 0 --no-pager -o cat | grep -E 'Oops|BUG:|Unable to handle|Kernel panic|WARNING: CPU|Internal error|Call trace' | sed 's/^.* at //' | tr '\n' ';')"
EOF
) || { echo "cannot reach $HOST"; exit 1; }

get() { printf '%s\n' "$r" | sed -n "s/^$1=//p"; }
fails=0
check() {  # check NAME OK? DETAIL
    if [ "$2" = 1 ]; then printf 'ok    %-14s %s\n' "$1" "$3"; else printf 'FAIL  %-14s %s\n' "$1" "$3"; fails=$((fails + 1)); fi
}
t() { [ "$@" ] && echo 1 || echo 0; }

trial=$(get trial)
echo "kernel: $(get uname_r) $(get uname_v), up $(get uptime_s) s, $([ "$trial" -ge 1 ] && echo TRIAL || echo normal) boot"
if [ -n "$build" ]; then
    want_v=$(sed -n 's/^uname -v: *//p' "$build/build-info.txt")
    check build "$(t "$(get uname_v)" = "$want_v")" "uname -v $(get uname_v) (build: $want_v)"
    want_c=$(shasum -a 256 "$build/config" | cut -d' ' -f1)
    check config "$(t "$(get config_sha256)" = "$want_c")" "/proc/config.gz sha256 ${want_c:0:16}… $( [ "$(get config_sha256)" = "$want_c" ] && echo matches || echo DIFFERS) BUILD_DIR/config"
fi
check release "$(t "$(get uname_r)" = 4.9.232)" "uname -r $(get uname_r)"
check dl-mode "$(t "$(get download_mode)" = 0)" "msm_poweroff download_mode=$(get download_mode)"
if [ "$trial" -ge 1 ]; then
    check switch "$(t "$(get switch)" = active -o "$(get trial_ok_flag)" = yes)" "kernel-trial.service $(get switch), /run/sonic-trial-ok: $(get trial_ok_flag)"
fi
check wifi "$(t "$(get wlan0)" = up -a "$(get wlan0_ipv4)" -ge 1)" "wlan0 $(get wlan0) ($(get wlan0_driver)), IPv4 addresses: $(get wlan0_ipv4), connman: $(get connman)"
check bluetooth "$(t "$(get hci0)" = present -a "$(get bt_powered)" = yes -a "$(get bluetoothd)" = active)" "hci0 $(get hci0), powered: $(get bt_powered), bluetoothd: $(get bluetoothd)"
check ioslink "$(t "$(get ioslink)" = active)" "service $(get ioslink); this boot: $(get ioslink_connects) iPhone connection(s), ANCS subscribed $(get ioslink_ancs_subscribed)x"
check display "$(t "$(get fb0)" != missing -a "$(get mdss_fb_bound)" -ge 1 -a "$(get dsi_bound)" -ge 1)" "fb0 $(get fb0), mdss_fb bound: $(get mdss_fb_bound), dsi bound: $(get dsi_bound), backlight $(get backlight)/255"
check touch "$(t "$(get touch_input)" -ge 1 -a "$(get touch_bound)" = yes)" "bt541_ts input: $(get touch_input), driver bound: $(get touch_bound), IRQs so far: $(get touch_irqs)"
check launcher "$(t "$(get launcher)" = active)" "asteroid-launcher $(get launcher)"
# The release kernel itself prints these two WARNINGs at every boot.
known='sysfs_warn_dup|pm_qos_add_request'
oops=$(get klog_oops | tr ';' '\n' | grep -v '^$' || true)
new_oops=$(printf '%s\n' "$oops" | grep -v -E "$known" | grep -v '^$' || true)
check oops "$(t -z "$new_oops")" "oops/BUG/WARN lines: $(printf '%s\n' "$oops" | grep -c . || true) ($(printf '%s\n' "$new_oops" | grep -c . || true) beyond the 2 known boot WARNINGs)"
[ -n "$new_oops" ] && printf '      %s\n' "$new_oops"
printf 'info  %-14s %s\n' klog "this boot: $(get klog_err) err, $(get klog_warn) warning-or-worse lines (compare with klog-compare.py)"
printf 'info  %-14s %s\n' failed-units "$(get failed_units)"

if [ "$fails" -gt 0 ]; then
    echo "RESULT: $fails check(s) FAILED"
    exit 1
fi
echo "RESULT: healthy"
if [ "$confirm" = 1 ]; then
    if [ "$trial" -ge 1 ]; then
        ssh -o ConnectTimeout=20 "$HOST" 'touch /run/sonic-trial-ok && ls -la /run/sonic-trial-ok'
        echo "confirmed: the dead-man's switch will leave this trial kernel running"
    else
        echo "not on a trial kernel; nothing to confirm"
        exit 2
    fi
fi
