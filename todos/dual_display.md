# Dual display: FSTN "Essential mode" screen (to do)

Status: research done, 2026-10-29. Nothing has been changed on the watch yet. The next step is test T2 below.

## Summary

- **Who drives what.** The AP never drives the FSTN.
  - The monochrome FSTN layer and its RGB backlight belong to the **sensor-hub MCU** ("nanohub").
  - The AP tells the MCU who owns the screen through four "mode" GPIOs, and sends settings (time, date order, backlight colour) as nanohub config packets over SPI.
- **The hand-over is already in the kernel.** When the AMOLED is **powered down** (`FB_BLANK_POWERDOWN`) and `lcd_mutex` is 1, the nanohub driver tells the MCU to take over (`AMBIENT`). AsteroidOS sets `lcd_mutex=1` at boot.
- **Why resting never shows the FSTN.** Always-on display is on. AsteroidOS then never powers the AMOLED down when idle; it puts it into doze-suspend (panel state LP2), and the driver counts that as "AP still owns the screen".
  - Stock Wear OS had exactly the same rule. Mobvoi's own description of the setting: *"Essential mode will light up when 'always-on screen' is off."*
- **Backlight colour.** It's a Pro 3 **Ultra**-only MCU setting: one RGB332 colour byte plus a brightness %. Mobvoi's picker offers 18 presets; the default is white at 20 %. AsteroidOS has no way to set it today, and the MCU forgets it on every AP boot.
- **No kernel change is needed.** The work is userspace: choosing when to use the FSTN, setting the colour, and fixing a time-sync storm found on the way (the FSTN clock is re-synced on almost every resume).

## Current state (2026-10-29), with evidence

| What | Evidence |
|---|---|
| AOD is on | dconf `/org/asteroidos/settings/always-on-display=true`; `mcetool`: `Use low power mode: enabled` |
| mce's "off" is really doze-suspend | at 20:57 and 21:12, `mcetool` says `Display state: off` while `/sys/class/graphics/fb0/show_blank_event` says `panel_power_on = 3` (= `MDSS_PANEL_POWER_LP2`) |
| The hand-over is armed | `/sys/class/nanohub/nanohub/lcd_mutex` = `1`, written at boot by AsteroidOS ([meta-rubyfish android-init `init.rc`](https://github.com/AsteroidOS/meta-smartwatch/blob/master/meta-rubyfish/recipes-android/android-init/android-init/init.rc): `write /sys/class/nanohub/nanohub/lcd_mutex 1`). This is the "Fix LCD" of meta-smartwatch PR #283 |
| The MCU drives the FSTN | every boot logs `nanohub: 281 [LCD-D] init start.` / `init completed.` |
| MCU time sync works | `[Time] sensorCfgDataTime: new deltatime = 1793300296, tm_gmtoff = 3600, 24h` (CET, 24 h) |
| Time-sync storm | boot of 19:58, after 72 min: 2,736 resumes, 344 `lcd-sync-time` runs for `ceres` (646 across all user managers), 1,877 `Start request repeated too quickly`. See "Time-sync storm" below |
| MCU firmware | hw type `0x4d71`, variant `0x22011110`: the last stock build (Wear OS PMRL.220111.001). AsteroidOS doesn't update it; stock ran `nanoapp_cmd download` at every boot |
| `lcd-tools` (2.1) | its units only run `--sync-time` (`SyncTime`). The binary also has step/HR/motion switches and `--prepare-timepiece`. `--session-restart` is a no-op on rubyfish, and there's no colour, tilt backlight or date order. Upstream: "Only the time synchronization functionality has been tested" ([lcd-tools #9](https://github.com/AsteroidOS/lcd-tools/pull/9)) |

## How the dual display works

### Hardware

- **AMOLED:** ICNA3310, command mode, driven by the AP through mdss. `/sys/class/leds/lcd-backlight` is this panel's brightness (DCS `0x51`); see [brightness_fix.md](brightness_fix.md) on the `brightness-fix` branch. It is *not* the FSTN backlight.
- **FSTN LCD and its RGB backlight:** driven by the MCU (STM32-class).
  - The AP has no framebuffer, LED device or `/dev` node for either. `fb1` is the mdss writeback display.
  - In Mobvoi's code, the MCU channel that takes the backlight and tilt settings is called `SpiOled`.
- **Links between the AP and the MCU** (`rover-nanohub.dtsi`, `rover-mdss-panels.dtsi`, checked in the live DT):

| Line | GPIO | Use |
|---|---|---|
| SPI (`spi_8`, 8 MHz), CS | 98 | nanohub packets (`/dev/nanohub*`) |
| irq1 / irq3 / wakeup | 61 / 59 / 63 | MCU interrupts, AP→MCU wake |
| nreset / boot0 | 70 / 65 | MCU reset and bootloader |
| mode-flag1..4 | 66, 60, 68, 69 | 4-bit mode code, latched by a pulse on… |
| hub-flag-int | 64 | …this interrupt to the MCU |
| `disp_mutex` (MCU→AP) | 133 | MCU busy with the display; unblank waits for it |
| `disp_en` ("lcd loadswitch") | 55 | display enable, driven by Mobvoi's mdss code |

- **TWM is unrelated.** mdss's TWM hooks (`twm_enable`, "TWM Enabled skip backlight update") are for Qualcomm's BlackGhost co-processor. All `qcom,bg*` nodes are `disabled` on rover and `twm_enable` reads 0.

### The hand-over in the kernel

Source: `mobvoi-ticwatch-kernel` @ `c428ef36`, `drivers/staging/nanohub/` and `drivers/video/fbdev/msm/mdss_fb.c`.

- **Mode codes** (`enum AP_GPIO_CMD`, sent by `__nanohub_set_mode_pin`; any code can also be sent by writing it to `/sys/class/nanohub/nanohub/mode`):

| Code | Name | Sent when |
|---|---|---|
| 0 | POWEROFF | AP shutdown (default) |
| 1 | BAND | before shutdown into Essential mode; makes the shutdown hook send BAND instead of POWEROFF |
| 2 | **AMBIENT** | AMOLED powered down and `lcd_mutex=1`: **MCU shows the FSTN** |
| 3 | NORMAL | probe, and any unblank / LP1 / LP2 blank while `lcd_mutex=1` |
| 6 / 7 | SUSPEND / RESUME | AP system suspend / resume |
| 5, 8 | fuel gauge requests | |
| 16 | RESEND | the MCU asks again (irq3) |

- **The rule** (`spi.c` `fb_notifier_callback`; it does nothing when `lcd_mutex` is 0):
  - `FB_EVENT_BLANK` with `FB_BLANK_POWERDOWN` → AMBIENT.
  - `FB_EARLY_EVENT_BLANK` with `UNBLANK`, `NORMAL` (LP1, doze) or `VSYNC_SUSPEND` (LP2, doze-suspend) → NORMAL.
  - So **a dozing AMOLED never hands over**.
- **mdss side:**
  - Unblank and LP1 first wait up to about 1 s while the MCU holds gpio 133 (`mdss_fb_mutex_with_ext_ctrl`), then raise gpio 55. Power-down drops gpio 55.
  - Writing `0` to the write-only `/sys/class/graphics/fb0/msm_fb_lcd_loadswitch` drops gpio 55 and makes every later unblank fail (`-EPERM`) until `1` is written. That's Mobvoi's "cut off screen".
- **The MCU is reset on every AP boot.** The nanohub kthread starts in `ST_RESET` and runs `nanohub_reset()`, so the MCU's volatile settings (colour, tilt backlight) are lost and must be re-applied after each boot. Stock re-applied them whenever its service started.

### Settings channel: nanohub config packets

Mobvoi's `libmcutool.so` (Google's `nanotool` plus Mobvoi additions; AsteroidOS ships the identical file at `/usr/libexec/hal-droid/system/lib/libmcutool.so`) writes one packet per setting to `/dev/nanohub`, in a single `write()`. I decoded it from the disassembly:

```
header, 20 bytes, little-endian (nanohub struct ConfigCmd):
  u32 evtType = 0x00000300   EVT_NO_SENSOR_CONFIG_EVENT
  u64 latency = 0
  u32 rate    = 0
  u8  sensorType             Mobvoi channel, below
  u8  cmd     = 3            CONFIG_CMD_CFG_DATA
  u16 flags   = 0
payload:
  time     (85): u8 0 | u64 unix time | i32 tm_gmtoff | u32 flags (bit0 = 24 h)   -> 37 bytes total
  SpiOled  (99): u32 cmd | u32 0 | u32 0 | u32 arg1 | u32 arg2                     -> 40 bytes total
                 cmd 7 = backlight colour (arg1 = RGB332, arg2 = brightness %)
                 cmd 9 = theater mode on, cmd 10 = theater mode off
  LcdApi   (91): larger packed struct; the date order uses bytes 00 02 01 <fmt> (not fully decoded)
```

- **Acknowledgement:** the kernel forwards the packet as `CMD_COMMS_WRITE`. `write()` returns the full length only if the MCU accepted it.
- **Other Mobvoi channels:** 86 ModeManager, 87 PwmMoter, 88 KeyManager, 89 CustomFlash, 98 PpgTouch.
- **The JNI functions** (`Java_com_mobvoi_ticwear_mcuservice_backend_CoreService_native*`, taking `(env, obj, args…)`; env is unused, so lcd-tools passes 0):

| Function | What it does |
|---|---|
| `AutoLowPowerScreen(b)` | writes `1`/`0` to `/sys/class/nanohub/nanohub/lcd_mutex` |
| `EnableLowPowerScreen(b)` | writes `2` (AMBIENT) / `3` (NORMAL) to `…/nanohub/mode` |
| `CutOffScreen()` | writes `0` to `/sys/class/graphics/fb0/msm_fb_lcd_loadswitch` |
| `BandMode()` | writes `1` (BAND) to `…/nanohub/mode` |
| `SyncTime()` | time packet; reads Android props `persist.sys.time_12_24` (and `persist.sys.timezone`, which is empty on AsteroidOS, harmless because libhybris maps `localtime` to glibc) |
| `ConfigOledColor(c, b)` | SpiOled cmd 7 |
| `EnableTheaterMode(b)` | SpiOled cmd 9 / 10 |
| `SyncDateFormat(n)` | LcdApi: 0 = MM/dd, 1 = dd/MM, 2 = other |
| `KeepOffBody(b)` | MCU stops reporting on-wrist while charging |
| `Enable{HeartRate,StepCounter,Motion}`, `SyncSteps`, `GetBandModeData`, `WipeBandModeData`, `AddAlarm`, `UpdateFitnessState` | fitness side |

### What stock Wear OS did

Mobvoi's `TicMcuService.apk` (`/android-root/system/priv-app/TicMcuService/`) does all of this; its `CoreService` is decompiled in my scratch folder.

- **"Essential mode" screen (Settings.Secure `low_power_screen`).**
  - On: `AutoLowPowerScreen(true)`, so the kernel hands over whenever the AMOLED powers down.
  - Turned on automatically once setup finished.
  - Its own summary: *"Once enabled, Essential mode will light up when 'always-on screen' is off."*
  - There was no separate "resting" detector. The FSTN showed whenever Wear OS turned the AMOLED off (timeout, wrist down, palm), and never while the Wear OS ambient screen was on.
- **"Tilt to light up backlight"** (sub-switches *Smart Mode* = `auto_tn_back_light_ap`, *Essential Mode* = `auto_tn_back_light`): sends `EnableTheaterMode(theater_mode_on || !switch)`. The MCU's own tilt detector (`[HandUp]` in its log) lights the backlight unless theater mode is on.
- **"Backlight color"** (only when the model is "TicWatch Pro 3 Ultra / 3U / 3X / Pro X").
  - It is stored as SharedPreference `custom_oled_color` = `"<colour>@<brightness%>"` and sent with `ConfigOledColor` at every service start and on change.
  - The 18 presets: `255@20` (default), `251@55`, `31@50`, `83@70`, `59@50`, `151@60`, `147@60`, `26@40`, `30@40`, `159@50`, `155@50`, `224@90`, `254@60`, `250@70`, `248@90`, `244@80`, `242@80`, `175@70`.
  - The colour byte is RGB332 (bits 7–5 red, 4–2 green, 1–0 blue). The picker's swatches match exactly, for example 224 = `#ee0000` red, 248 = `#eecc00` amber, 83 = `#4488cc` blue, 31 = `#01eecc` cyan, 175 = `#aa66cc` purple.
- **Time.** Synced on TIME_SET, TIMEZONE_CHANGED and DATE_CHANGED, plus an exact alarm at the next DST transition. Changing the 12/24 h setting re-synced it. The date order was sent separately with `SyncDateFormat`.
- **Essential (band) mode, with the AP off.**
  - Triggers: by hand, from the power-key menu, on a schedule, or automatically when unplugged at or below `custom_band_mode_level` (default 5 %) while the screen is off.
  - Sequence: `AutoLowPowerScreen(true)` → screen off (or `EnableLowPowerScreen(true)`) → `AutoLowPowerScreen(false)` → `CutOffScreen` → `EnableTheaterMode(!auto_tn_back_light)` → `WipeBandModeData` → `BandMode` → shutdown.

## Why resting doesn't switch to the FSTN today

With always-on display enabled, AsteroidOS blanks to doze-suspend:
- **The compositor:** the meta-asteroid 2.1-era `qt5-qpa-hwcomposer-plugin` patch "Add ambient mode display support" uses `HWC2_POWER_MODE_DOZE_SUSPEND` instead of `OFF` when ambient is enabled. `QPA_HWC2_FORCE_DOZE_UPDATE=1` (set by meta-rubyfish) flips the panel ON and back for each AOD redraw.
- **mce:** AsteroidOS mce patch 0004 uses `FB_BLANK_VSYNC_SUSPEND` instead of `POWERDOWN` when low-power mode is on.
- **The result:** the panel sits in LP2, the nanohub driver sends NORMAL, and the MCU never takes over. AOD redraws even send NORMAL twice a minute.

With AOD **off**, the same code paths use `HWC2_POWER_MODE_OFF` / `FB_BLANK_POWERDOWN`. Since `lcd_mutex` is already 1, the FSTN should take over when the screen goes off, with no change at all. That's untested; it's test T2.

Note that **cinema mode** in the quick panel turns AOD off, so it should also bring up the FSTN, with tilt backlight unless theater mode is on.

## What's missing

### Kernel

- **Nothing is required.** `lcd_mutex`, the fb hook, the mode lines, the display mutex and the load switch are all there and armed.
- **Optional, only if a kernel is being built anyway** (the [brightness_fix.md](brightness_fix.md) / [touch_driver_fix.md](touch_driver_fix.md) build, `fastboot boot` first), about 30 min, low risk:
  - Give `/sys/class/nanohub/nanohub/mode` a read handler printing `hub_mode_ap_active` / `hub_mode_ap_pwr_down`. Today userspace can't see whether the MCU owns the screen; the MCU's own `[mode] apCmd` log stops reaching dmesg once the sensor HAL is up.
  - Make `fb_notifier_callback` ignore framebuffers other than fb0. It reacts to every fb today, though nothing blanks fb1.
- **Interplay with the touch bug.** With AOD off, a screen-off is `POWERDOWN`, so the touch controller suspends with the display. The race in [touch_driver_fix.md](touch_driver_fix.md) (on the `touch-driver-fix` branch) needs the chip active during system suspend, so it should get rarer. The fix is still needed, because AOD, nightstand and resume paths remain.

### Userspace

1. **A policy for FSTN versus AMOLED AOD.** The simplest is AOD off, which is stock's default behaviour. A smarter policy needs a small daemon.
2. **Backlight colour.** Nothing sets it, and the MCU forgets it on each AP boot. It needs a setter, persistence and re-applying at boot.
3. **Tilt backlight / theater mode.** Not exposed. The MCU's state after reset is unknown. Cinema mode doesn't tell the MCU.
4. **Date order on the FSTN.** Not set; it's the same problem as upstream [meta-smartwatch #319](https://github.com/AsteroidOS/meta-smartwatch/issues/319) on catfish. For CET, dd/MM is `SyncDateFormat(1)`.
5. **Time-sync storm.** `lcd-sync-time.timer` has `OnClockChange=true`.
   - On this 4.9 kernel every resume from suspend calls `clock_was_set_delayed()` → `timerfd_clock_was_set()`, which systemd reports as a clock change. So the MCU clock is re-synced after nearly every resume: libhybris loads, the MCU wakes, and systemd hits its start limit.
   - Every `systemd --user` instance runs its own copy, including root's from SSH logins.
   - ioslink is not the cause: it only sets the clock when it drifts by 3 s or more.
   - Removing the trigger needs a replacement for DST changes, which today are only picked up by accident.
6. **Essential (band) mode.**
   - `lcd-tools --prepare-timepiece` exists, but upstream's newer `asteroid-second-display` hides it on rubyfish "as it would just power off the watch".
   - Unlike stock, lcd-tools never sends AMBIENT before BAND. That may be why.
7. **Upstream home.** [asteroid-second-display](https://github.com/MagneFire/asteroid-second-display) (Qt6, a D-Bus daemon replacing lcd-tools, not in 2.1) has a rubyfish backend with time sync and step/HR/motion only. Colour, theater, date order and the timer fix belong there.

## Plan

### (a) Switch to the battery-saving screen when resting or idle

| # | Step | Effort | Risk |
|---|---|---|---|
| a1 | **Test with AOD off** (T1–T2). That's Settings → Display → Always on display, or the quick-panel AOD toggle. Confirm the FSTN shows when the screen goes off, `panel_power_on = 0`, and the wake-up is clean. Revert by turning AOD back on | 10 min | none |
| a2 | **If it works, keep AOD off.** That gives stock behaviour: the FSTN whenever the AMOLED is off, including after the top-button blank. The trade-off is no AMOLED ambient face. Do the same for nightstand mode's own AOD key, `/desktop/asteroid/nightstand/always-on-display`, if you want the FSTN on the charger | 0 | none |
| a3 | **Stop the time-sync storm** (T3). Add a timer drop-in with `OnClockChange=false` plus an hourly sync, which also catches DST within the hour. Optionally have ioslink start `lcd-sync-time.service` after it sets the clock | 15 min | low; revert by deleting the drop-in |
| a4 | **Optional smarter policy**, a small user daemon written in Go like ioslink. It would keep AMOLED AOD while you're active and switch to the FSTN when charging, at night, below a battery level, or when the watch has been still for N minutes. It flips the same two settings the UI does: mce `/system/osso/dsm/display/use_low_power_mode` via `com.nokia.mce` `set_config`, and dconf `always-on-display`. Check first whether mce re-blanks a dozing panel to `POWERDOWN` when the setting changes mid-doze (open question 7) | 1–2 days | low–medium |
| a5 | **Optional Essential (band) mode** (T6). Try stock's sequence, which sends AMBIENT before BAND, then power off. If it works, wire a low-battery trigger such as "≤ 5 % and screen off" into the a4 daemon | 1 h to test | medium: the watch powers off; recover with a long press |

### (b) Choose the FSTN backlight colour

| # | Step | Effort | Risk |
|---|---|---|---|
| b1 | **Framing check without building anything** (T4). Send a raw *time* packet with `dd` and look for the MCU's `[Time] sensorCfgDataTime` echo in dmesg. This proves the reverse-engineered packet format on a harmless command lcd-tools already sends hundreds of times a day | 10 min | low |
| b2 | **Colour by raw packet** (T5). Send a preset such as `224@90`, then bring up the FSTN (T2) and tilt or cover the watch to light it. Also try theater off (cmd 10) if tilting doesn't light it | 10 min | low–medium: the format is reverse-engineered. If the MCU chokes, the nanohub driver resets it after repeated errors, and a reboot always clears it |
| b3 | **Or use stock's own code path** (T7): the `mcuctl` helper below, which calls `libmcutool` through libhybris exactly as stock and lcd-tools do, cross-compiled in Docker | 1–2 h | low |
| b4 | **Persist.** Keep the choice in dconf, for example `/org/asteroidos/settings/fstn-backlight = '224@90'` in Mobvoi's format, plus `fstn-tilt` and `fstn-date-order`. Apply them from a `ceres` user oneshot at session start (`After=lcd-session-restart.service`, `WantedBy=basic.target`), because the MCU is reset at every AP boot. Gate it on `getprop ro.product.device` = `rover`, since only the Ultra has the RGB backlight | 1 h | low |
| b5 | **Optional UI and upstream.** Add a colour picker with Mobvoi's 18 presets (asteroid-settings page or quick-panel entry). Upstream, add `BacklightColor` / tilt / date-order capabilities to asteroid-second-display's Mobvoi backend (rover only) and fix its `OnClockChange` timer the same way. That targets the Qt6 nightly, not 2.1 | days | low |

### `mcuctl` helper for b3

Build it on the Mac with `docker run --rm -v "$PWD":/w -w /w debian:bookworm sh -c 'apt-get update -qq && apt-get install -y -qq gcc-arm-linux-gnueabihf >/dev/null && arm-linux-gnueabihf-gcc -O2 -Wall -o mcuctl mcuctl.c'`. That uses glibc 2.36; the watch has 2.41.

```c
/* mcuctl.c: call Mobvoi's libmcutool (stock's own code) through libhybris. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P "Java_com_mobvoi_ticwear_mcuservice_backend_CoreService_native"
typedef void *(*hopen_t)(const char *, int);
typedef void *(*hsym_t)(void *, const char *);
typedef int (*f0_t)(void *, void *);
typedef int (*f1_t)(void *, void *, int);
typedef int (*f2_t)(void *, void *, int, int);

int main(int argc, char **argv)
{
	void *hc = dlopen("libhybris-common.so.1", RTLD_NOW);
	hopen_t hopen = hc ? (hopen_t)dlsym(hc, "hybris_dlopen") : NULL;
	hsym_t hsym = hc ? (hsym_t)dlsym(hc, "hybris_dlsym") : NULL;
	void *lib = (hopen && hsym) ? hopen("libmcutool.so", RTLD_LAZY) : NULL;
	void *f = NULL;
	int r = 0;

	if (!lib) {
		fprintf(stderr, "cannot load libmcutool.so through libhybris\n");
		return 1;
	}
	if (argc == 4 && !strcmp(argv[1], "color") && (f = hsym(lib, P "ConfigOledColor")))
		r = ((f2_t)f)(NULL, NULL, atoi(argv[2]), atoi(argv[3]));  /* RGB332, brightness % */
	else if (argc == 3 && !strcmp(argv[1], "theater") && (f = hsym(lib, P "EnableTheaterMode")))
		r = ((f1_t)f)(NULL, NULL, atoi(argv[2]));                  /* 1 = no tilt backlight */
	else if (argc == 3 && !strcmp(argv[1], "datefmt") && (f = hsym(lib, P "SyncDateFormat")))
		r = ((f1_t)f)(NULL, NULL, atoi(argv[2]));                  /* 0 = MM/dd, 1 = dd/MM */
	else if (argc == 2 && !strcmp(argv[1], "synctime") && (f = hsym(lib, P "SyncTime")))
		r = ((f0_t)f)(NULL, NULL);
	else {
		fprintf(stderr, "usage: mcuctl color <0-255> <percent> | theater 0|1 | datefmt 0|1 | synctime\n");
		return 64;
	}
	printf("libmcutool returned %d\n", r);  /* not meaningful: SyncTime returns 1 on success */
	return 0;
}
```

## Test commands (for you to run, in order)

Run T1 first; it's read-only.

**T1. Baseline**
```
ssh sonic-screwdriver 'mcetool | grep -E "^Display state|^Use low power mode"; cat /sys/class/graphics/fb0/show_blank_event /sys/class/nanohub/nanohub/lcd_mutex'
# today: "off" / "enabled" / "panel_power_on = 3" (LP2, doze) / 1
```

**T2. AOD off: does the FSTN take over?** These commands do the same as the Settings switch.
```
ssh sonic-screwdriver 'mcetool --set-low-power-mode=disabled'
ssh sonic-screwdriver 'su ceres -c "XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus dconf write /org/asteroidos/settings/always-on-display false"'
# press the top button once (or wait for the timeout), wait 2 s, then:
ssh sonic-screwdriver 'mcetool | grep "^Display state"; cat /sys/class/graphics/fb0/show_blank_event; grep -E "gpio(55|133) " /sys/kernel/debug/gpio'
```
- **Expected:** `off`, `panel_power_on = 0`, and the time on the FSTN.
- **Things to note:**
  - what the FSTN shows (time, date, battery, steps?);
  - whether tilting lights its backlight, and in which colour;
  - that it still ticks after 10+ min of deep sleep;
  - wake-up with the top button: AMOLED back, FSTN cleared, how long it takes, and whether tap-to-wake still works.
- **Revert:** `mcetool --set-low-power-mode=enabled` and write `true` to the same dconf key.

**T3. Stop the time-sync storm.** Takes effect after a reboot. Undo by deleting the file.
```
ssh sonic-screwdriver 'mkdir -p /etc/systemd/user/lcd-sync-time.timer.d && printf "[Timer]\nOnClockChange=false\nOnCalendar=hourly\nAccuracySec=1min\n" > /etc/systemd/user/lcd-sync-time.timer.d/10-no-resume-storm.conf'
# after the reboot and an hour of normal use, expect a handful, not hundreds:
ssh sonic-screwdriver 'journalctl -b | grep -c "Starting Sync LCD time"; journalctl -b | grep -c "Start request repeated too quickly"'
```

**T4. Raw packet framing check (time).** Build the packet on the Mac:
```
python3 -c 'import struct,sys,time; sys.stdout.buffer.write(struct.pack("<IQIBBH",0x300,0,0,85,3,0)+struct.pack("<BQiI",0,int(time.time()),time.localtime().tm_gmtoff,1))' > /tmp/fstn-time.bin
scp /tmp/fstn-time.bin sonic-screwdriver:/tmp/ && ssh sonic-screwdriver 'dd if=/tmp/fstn-time.bin of=/dev/nanohub bs=37 count=1; sleep 1; dmesg | grep sensorCfgDataTime | tail -1'
# expect "37 bytes … copied" and a fresh "[Time] sensorCfgDataTime: new deltatime = ~1793300296 (±2), tm_gmtoff = 3600, 24h"
```

**T5. Backlight colour by raw packet.** With the FSTN up (T2), send red at 90 %:
```
python3 -c 'import struct,sys; sys.stdout.buffer.write(struct.pack("<IQIBBH",0x300,0,0,99,3,0)+struct.pack("<5I",7,0,0,224,90))' > /tmp/fstn-red.bin
scp /tmp/fstn-red.bin sonic-screwdriver:/tmp/ && ssh sonic-screwdriver 'dd if=/tmp/fstn-red.bin of=/dev/nanohub bs=40 count=1'
```
- Tilt the wrist, or cover the watch in the dark, to light the backlight; it should be red.
- If nothing lights up, turn theater mode **off** with the same command and `struct.pack("<5I",10,0,0,0,0)`. Theater mode on is `9`.
- Back to the default: `struct.pack("<5I",7,0,0,255,20)`.

**T6. Optional: Essential (band) mode, stock's order.** The watch powers off; boot it with a long press.
```
ssh sonic-screwdriver 'echo 1 > /sys/class/nanohub/nanohub/lcd_mutex; echo 2 > /sys/class/nanohub/nanohub/mode; sleep 1; echo 0 > /sys/class/nanohub/nanohub/lcd_mutex; echo 0 > /sys/class/graphics/fb0/msm_fb_lcd_loadswitch; echo 1 > /sys/class/nanohub/nanohub/mode; poweroff'
# expect the FSTN to keep showing the time (and steps/heart rate) with the AP off
```
This skips `WipeBandModeData`, which only clears Mobvoi's band-mode health log. Writing `0` to `msm_fb_lcd_loadswitch` blocks AMOLED unblank until reboot, so only run this as the last command before power-off.

**T7. Stock code path (`mcuctl`).**
```
scp mcuctl sonic-screwdriver:/home/ceres/bin/
ssh sonic-screwdriver 'su ceres -c "/home/ceres/bin/mcuctl color 224 90; /home/ceres/bin/mcuctl datefmt 1"'
```

## Open questions

1. **What does the FSTN show in AMBIENT mode while the AP runs?** It might be time only, or date, battery, steps and heart rate. Step and HR data on the FSTN depend on `Enable{StepCounter,HeartRate,Motion}`; lcd-tools has `--enable-stepcounter` and similar.
2. **What is the MCU's theater/tilt-backlight state after its boot-time reset?** Also, AsteroidOS's tilt-to-wake (mce patch 0002) may wake the AMOLED on the same gesture that lights the FSTN backlight; decide which one you want.
3. **Which colour and brightness values are valid?** Mobvoi only offers the 18 presets, with brightness 20–90 %. Other RGB332 values and brightness up to 100 % are untested.
4. **Wake latency from POWERDOWN.** Unblank waits for the MCU's mutex line (up to about 1 s) and then fully re-initialises the panel. Is the delay noticeable compared with waking from doze?
5. **What does gpio 55 actually switch?** It reads low while the panel is in LP2, which the code doesn't explain. It only matters for `CutOffScreen` and Essential mode.
6. **Is Essential mode viable on rover?** Does T6 keep the FSTN alive with the AP off, and how does the watch exit band mode? Stock says to long-press power until the boot screen.
7. **Does mce move a dozing panel to `POWERDOWN` immediately** when low-power mode is turned off mid-doze, or only at the next blank? This matters for a4.
8. **Do the FSTN's needs fit the existing sensor setup?** `/dev/nanohub` is shared with sensorfwd's HAL. Config writes need no lock and work today (lcd-tools does them constantly), but a raw-packet daemon should keep writes rare.

Research notes, the decompiled TicMcuService, the disassembly, and the copies of lcd-tools, asteroid-second-display and the AsteroidOS patches are in `~/.cache/sonic-screwdriver/research-display/` (start with `NOTES.md`).
