#!/bin/bash
# sonic/panel-page-check.sh: after full brightness, does the panel take
# brightness changes again? For kernels with the boost-mode-out fix
# (mdss_dsi_cmds_end_on_user_page, mdss_dsi_panel.c). Runs on the watch as
# root, unattended, started detached so a suspend or a dropped SSH can't cut
# it short; its lines go to the journal:
#
#   ssh sonic-screwdriver 'cat > /tmp/sonic-panel-page-check.sh' < sonic/panel-page-check.sh
#   ssh sonic-screwdriver systemd-run --unit=sonic-panel-page-check --collect bash /tmp/sonic-panel-page-check.sh
#   ssh sonic-screwdriver journalctl -u sonic-panel-page-check -o short-precise --no-pager
#
# What it does, through mce as the flashlight does:
#   1. holds a timed wakelock, turns the screen on;
#   2. sets brightness 100 %: the LED must reach 255 and hbm_mode 1 (the
#      kernel sent "boost mode in"), else the result is INCONCLUSIVE;
#   3. sets the user's brightness again: the LED must drop below 255 and
#      hbm_mode read 0 (the kernel sent "boost mode out", then 51 <level>);
#   4. reads two panel registers through debugfs (DCS reads): FFh, the
#      command page the panel has selected, and 52h, its brightness when that
#      page is 0;
#   5. selects page 0 by hand, with FE 00 (the stock page select, the one the
#      fix appends to boost mode out), and reads both again: on page 0, 52h is
#      the brightness, whatever page boost mode out left. That's the only
#      write to the panel.
# PASS: page 00 and 52h equal to the LED level, before and after step 5.
# Without the fix, step 4 finds the page boost mode out left (expected 70)
# and step 5 finds the brightness stuck at ff. The user's brightness setting
# ends as it was, and the screen is blanked again, which powers the panel
# off: the next screen-on starts it afresh. Nothing sounds or buzzes, and
# Wi-Fi and Bluetooth are left alone.
set -u
LED=/sys/class/leds/lcd-backlight/brightness
HBM=/sys/class/graphics/fb0/hbm_mode
DBG=/sys/kernel/debug/mdp
log() { echo "$(date +%T.%3N) $*"; }
result() { log "RESULT: $*"; exit 0; }
mce() { # mce <method> [args]: a request to mce, no reply
	local m=$1
	shift
	dbus-send --system --type=method_call --dest=com.nokia.mce /com/nokia/mce/request "com.nokia.mce.request.$m" "$@"
}
reg() { # reg <hex register>: one byte read from the panel, as two hex digits
	echo "$1 1" >"$DBG/panel_off" || return 1
	sed -n 's/^0x[0-9a-f]*: 0x\([0-9a-f][0-9a-f]\)$/\1/p' "$DBG/panel_reg"
}
settle() { # settle <seconds>: wait until the LED level holds for 0.5 s
	local end=$((SECONDS + $1)) a b
	a=$(cat $LED)
	while [ $SECONDS -lt $end ]; do
		sleep 0.5
		b=$(cat $LED)
		[ "$a" = "$b" ] && break
		a=$b
	done
}

setting=$(mcetool | sed -n 's/^Brightness: *\([0-9]*\) .*/\1/p')
[ -n "$setting" ] || { log "RESULT: INCONCLUSIVE: no brightness setting from mcetool"; exit 0; }
echo "sonic_panel_check $((90 * 1000000000))" >/sys/power/wake_lock
cleanup() {
	mcetool --set-display-brightness="$setting" >/dev/null
	mce req_display_cancel_blanking_pause
	mcetool --blank-screen >/dev/null
	echo sonic_panel_check >/sys/power/wake_unlock
}
trap cleanup EXIT

log "kernel $(uname -v); brightness setting $setting %; LED $(cat $LED), hbm_mode $(cat $HBM)"
mce req_display_state_on
mce req_display_blanking_pause
mce req_tklock_mode_change string:unlocked
sleep 2
log "screen on: $(mcetool | sed -n 's/^Display state: *//p'); LED $(cat $LED), page $(reg ff), 52h $(reg 52)"

mcetool --set-display-brightness=100 >/dev/null
settle 4
led=$(cat $LED) hbm=$(cat $HBM)
log "100 %: LED $led, hbm_mode $hbm"
[ "$led" = 255 ] && [ "$hbm" = 1 ] || result "INCONCLUSIVE: 100 % didn't reach LED 255 with hbm_mode 1"

mcetool --set-display-brightness="$setting" >/dev/null
settle 4
led=$(cat $LED) hbm=$(cat $HBM)
log "back to $setting %: LED $led, hbm_mode $hbm"
[ "$led" -lt 255 ] && [ "$hbm" = 0 ] || result "INCONCLUSIVE: the LED stayed at 255 or hbm_mode at 1"

page=$(reg ff) b52=$(reg 52)
want=$(printf '%02x' "$led")
log "panel as boost mode out left it: page $page, 52h $b52; LED $led (0x$want)"
echo "fe 00" >"$DBG/panel_reg"
page0=$(reg ff) b52p0=$(reg 52) led2=$(cat $LED)
log "after FE 00 by hand: page $page0, 52h $b52p0; LED $led2"
[ "$led2" = "$led" ] || result "INCONCLUSIVE: the LED moved during the reads ($led -> $led2)"
[ "$page0" = 00 ] || result "INCONCLUSIVE: FE 00 by hand didn't select page 0 (page $page0)"
if [ "$page" = 00 ] && [ "$b52" = "$want" ] && [ "$b52p0" = "$want" ]; then
	result "PASS: boost mode out left page 00, and the panel took the brightness (52h $b52 = LED $led)"
fi
result "FAIL: boost mode out left page $page (want 00); the brightness on page 0 is $b52p0 (want $want)"
