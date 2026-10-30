# Brightness fix

Make automatic brightness work on the watch:

- Fix a kernel bug that holds back brightness changes. **Done**, but it turned out not to cause the symptoms below.
- Find what does cause them. Most likely userspace (mce), see [What is still open](#what-is-still-open).
- Then retune the brightness curve to suit me.

Status (2026-10-30):

- **Kernel: fixed** on this branch by commit `a8a8df54eaf9`, "msm: mdss: lift the backlight hold at the first frame after unblank". Trial build 004 (`#4-a8a8df5`) ran from `recovery` with no regressions: the `sonic/BUILD.md` checks, the kernel log, and wake and always-on cycles.
- **But the bug is latent on AsteroidOS 2.1.** mce always sets a brightness level between unblanking the panel and the first frame, so that frame lifts the hold anyway. On the unfixed kernel, none of 26 writes on a static screen after a wake were held back. The fix changes nothing the watch does today. It closes a race that other timings or other userspace could hit.
- **The symptoms are still open.** The 2026-10-29 evidence for the kernel bug doesn't hold up, see [below](#what-the-2026-10-29-evidence-really-showed).

## Symptoms

- Writing to `/sys/class/leds/lcd-backlight/brightness` seems to do nothing.
- The Settings brightness slider seems to jump between only two levels, "bright" and "dimmer".
- Automatic brightness looks erratic and generally too dim.

## The panel

The panel and the kernel's brightness path work.

- **Panel:** Chipone **ICNA3310** AMOLED, command mode (`msm_fb_panel_info`).
- **Brightness command:** DCS `0x51 <level>`, one byte, range 1–255 (`qcom,mdss-dsi-bl-pmic-control-type = bl_ctrl_dcs`). The panel's start-up commands enable it (`fe 00`, `51 ff`, `53 20`), so the panel accepts it.
- **Always-on (ambient) mode** puts the panel in idle mode (DCS `0x39`) and the framebuffer in LP2 (`show_blank_event` = 3). mce then writes its LPM level (47/255 here), and the kernel sends it to the panel. Whether the panel honours `0x51` in idle mode has to be checked by eye.

## What the 2026-10-29 evidence really showed

The `6014 -> 6014` check (a `bl_event` that doesn't move after a write) doesn't prove a hold-back:

- **`bl_event` isn't counted while the framebuffer is blanked or in a low-power state.** `mdss_fb_bl_update_notify()` returns early while `mfd->update.is_suspend` is set, which happens for any blank, always-on included. On 2026-10-29 the always-on display was on, so "screen off" meant LP2. A write there reaches the panel but doesn't move `bl_event`. Measured 2026-10-30 in ambient: the read-back went 39 → 47 while `bl_event` stayed the same.
- **A write while the panel is fully off is kept for the next wake**, by design. mce then sets its own level anyway.
- **mce re-applies its own level** at every wake, dim and ALS change, so a manual write lasts only until mce's next one.
- **Continuous redraws hide a hold-back.** On one boot on the charger, the launcher redrew at 60 fps the whole time the screen was on (cause unclear; after a reboot it didn't come back).

**A correct check:**

1. The screen must be on: `show_blank_event` says `panel_power_on = 1`.
2. It must be static: the `play` counter in `/sys/kernel/debug/mdp/stat` doesn't move for a couple of seconds.
3. Then write a level, and compare the read-back and `bl_event`:

```sh
ssh sonic-screwdriver 'F=/sys/class/graphics/fb0; B=/sys/class/leds/lcd-backlight/brightness
  cat $F/show_blank_event; grep play /sys/kernel/debug/mdp/stat; sleep 2; grep play /sys/kernel/debug/mdp/stat
  b0=$(cat $B); v=$(( b0 > 128 ? b0 - 60 : b0 + 60 )); e0=$(cat $F/bl_event); echo $v > $B
  echo "wrote $v: read back $(cat $B), bl_event $e0 -> $(cat $F/bl_event)"; echo $b0 > $B'
```

Held back means the read-back keeps the old level and `bl_event` doesn't move. Applied means the read-back is the new level and `bl_event` went up by one. The read-back is `mfd->bl_level`, the level the kernel last sent.

## The kernel bug (fixed)

- **Source:** `mobvoi/mobvoi-ticwatch-kernel`, branch `mobvoi-android-msm-rover-4.9`, commit `c428ef3654d52e816308a6cf11009a1742f86c1c`. AsteroidOS's `meta-rubyfish` adds patches 0001–0006, none of which touch the display.
- **File:** `drivers/video/fbdev/msm/mdss_fb.c`.

How the hold is meant to work:

1. **On unblank** from off (`mdss_fb_blank_unblank()`), the driver sends any level stored while the panel was off, then sets `allow_bl_update = false`. The backlight must not change again before the first frame reaches the panel.
2. **While the hold is on**, `mdss_fb_set_backlight()` only stores the requested level in `unset_bl_level`.
3. **After each frame** (`__mdss_fb_perform_commit()`, the `skip_commit:` path, after a kickoff that succeeded), `mdss_fb_update_backlight()` sends a stored level and sets `allow_bl_update = true`. That is its only caller. The unblank's own frame doesn't go through it.

The bug: `mdss_fb_update_backlight()` returned early when no level was stored (`unset_bl_level == U32_MAX`), *without* lifting the hold. If no level is requested between unblank and the first frame, the next request waits for another frame. On a static watch face that can be a minute.

### Why AsteroidOS doesn't hit it

mce's debug log of one wake (2026-10-30):

1. mce unblanks the framebuffer itself (`mce_fbdev_set_power(up)`, about 270 ms).
2. Right after, `mdy_brightness_force_level(): brightness from 0 to 1`. The hold is on, so that level is stored.
3. Only then does mce ask the compositor to draw (`setUpdatesEnabled`), and once it answers, mce fades 1 → target.

So a level is always pending at the first frame, and that frame lifts the hold. Waking from always-on mode (LP2 → on) doesn't set the hold at all.

### The fix

Commit `a8a8df54eaf9`, for meta-rubyfish a `0007-…patch` (`git format-patch -1 a8a8df54eaf9`):

- At the first frame after unblank, lift the hold even when no level is pending.
- Decide under `bl_lock`. The patch first proposed here checked `unset_bl_level` without the lock, so a level stored in between would have been dropped.
- The lockless early return now tests `allow_bl_update`, so later frames still don't take the lock.
- Only `mdss_fb_update_backlight` changes, by 16 bytes of code. The config, device trees and all 9046 exported symbol CRCs are unchanged.

## Build and test

- **Build and trial-boot** exactly as in `sonic/BUILD.md` (on branch `asteroidos-2.1`). It replaces the plan that was here. Note that systemd 257 on the watch rejects `systemctl reboot recovery`; use `systemctl reboot --reboot-argument=recovery`.
- **Results, 2026-10-30.** Build 002 has the same code as the release; build 004 is this branch. "Writes" are two levels written after each wake on a static screen, then put back.

  | Test | Build 002 (unfixed) | Build 004 (fixed) |
  |---|---|---|
  | Power key, then unlock (like a button press and a touch) | 4 wakes, 8/8 writes applied at once | 3 wakes, 6/6 |
  | `mcetool --unblank-screen` (the earlier recipe) | 2 wakes, 4/4 | 2 wakes, 4/4 |
  | Automatic brightness off and/or instant unblank fade | 7 wakes, 14/14 | 2 wakes, 4/4 |
  | Always-on cycles: on → ambient → on | dims 89 → 47 in LP2; write after wake applied (2 cycles; the cable was pulled during the 2nd) | 5 cycles, the same |

  - In every wake the screen was static again about 1.2 s later. The tests changed mce settings only during the run and restored them.
  - The kernel log of the trial boot differs from the reference boots only by version strings and by the environment: cable unplugged, Wi-Fi, touch.
- **Flash to `boot`:** later, together with the touch and sound fixes, after the combined kernel has run from `recovery` for a day.
- **Upstream:** optional and low priority, since AsteroidOS doesn't hit the bug. If done: an issue and a `0007` patch for AsteroidOS/meta-smartwatch, `meta-rubyfish/recipes-kernel/linux/`.

## What is still open

The symptoms need a new diagnosis, probably in userspace:

- **"Writes do nothing":** see [above](#what-the-2026-10-29-evidence-really-showed). mce overrides manual writes; to try levels by hand, turn automatic brightness off first.
- **Slider with "two levels":** with automatic brightness on, the slider picks an ALS profile, not a level. At a given light level many profiles give similar brightness. This is the curve, below.
- **Too dim:** the curve, below. Also check how bright ambient (always-on) mode looks, since mce caps it at its LPM table (47/255 at ~90 lux).

### Related, not fixed here: dead panel at boot

Sometimes, right after boot, the first frame times out (`wait4pingpong timed out` → `Panel has gone bad … PANEL_ALIVE=0`, see `sonic/BUILD.md`).

- `panel_dead` then makes `__mdss_fb_perform_commit()` drop every frame, and brightness changes are held back.
- Both last until the next blank and unblank.
- Android's display HAL reacts to `PANEL_ALIVE=0` by power-cycling the display. Nothing in AsteroidOS does.
- If it shows up in daily use (a frozen screen right after boot), the fix belongs in userspace (blank/unblank on that uevent) or in the boot-time race itself.

## Retuning the automatic brightness curve (after the kernel fix)

### How it works

- **mce turns lux into brightness** using `/etc/mce/20als-defaults.ini`, section `[BrightnessDisplay]`.
- **The table has 21 profiles:** `LimitsProfileN` (lux steps) paired with `LevelsProfileN` (brightness %), for N = 0–20.
- **The brightness setting picks the profile:** 1% → profile 0, 50% → profile 10, 100% → profile 20.
- **Every profile reaches 100% only above 1,000–2,000 lux.**
- **Watch Settings app quirks:**
  - The "automatic brightness" switch controls mce's `als_enabled`.
  - With it on, the slider picks a profile, not an absolute brightness.

### The light sensor works

It's read through `sensorfwd` (`hybrisalsadaptor`) and only reports when the light changes, about 8 times a second.

| Situation | Measured |
|---|---|
| Evening room light, watch held normally | ~50–70 lux |
| Same room, pointed at the ceiling light | ~330–500 lux |
| Phone flashlight at 5 cm | ~2,000 lux |
| Covered | 0–17 lux |

### Calibration points so far

| Situation | Lux | Brightness I want | Default gives (setting 50%) |
|---|---|---|---|
| Normal room, evening | ~60 | 80% | 37–41% (even 100% gives only ~65%) |
| Lights off | todo | todo | |
| Next to a bright lamp | todo | todo | |
| Daylight / outdoors | todo | todo | |

### Procedure for each point

1. Turn on automatic brightness and mce's lux logging:
   ```
   ssh sonic-screwdriver 'mcetool --set-als-mode=enabled --set-als-autobrightness=enabled --set-verbosity=debug'
   ```
2. Hold the watch normally in that light for about 5 s. That's when the lux gets captured.
3. In Settings → Display, turn automatic brightness off, set the slider to taste, and note the %.
4. Read the lux from just before step 3:
   ```
   ssh sonic-screwdriver 'journalctl -o short-monotonic --since -300s | grep "ALS: UPDATE SENSORD" | tail -n 6'
   ```
5. When finished, run `ssh sonic-screwdriver 'mcetool --set-verbosity=warning'`.

### Applying the curve

- Write an override file, `/etc/mce/90-sonic-screwdriver-als.ini`, with a new `[BrightnessDisplay]` table. mce reads `/etc/mce/*.ini` in order, and later files override earlier keys.
- Make **profile 10** (setting 50%) match my calibrated curve, and shift the other profiles up or down from it, so the slider still adds a bias.
- Apply it with `systemctl restart mce` or a reboot.
- Undo it by deleting the file.

### Also noticed

- At every wake mce first sets level 1, then fades to its target over 90 ms (2026-10-30 debug log). The "darkest step until the first lux sample" seen before may be that. If it shows as a dim flash on wake, look at mce's `als` handling. Otherwise ignore it.
- **Nightstand mode.** While the watch charges, the launcher sets the brightness setting to its nightstand value (30%) and turns always-on off. It restores both 5 s after the watch comes off the charger. So the brightness setting seen while charging isn't the one in daily use.

## Current watch state (2026-10-30)

- **Automatic brightness:** on (`als_enabled` and `als_autobrightness` both enabled).
- **Brightness setting:** 30%. That is also the launcher's nightstand value, but it stayed at 30% off the charger too.
- **Always-on display:** off (`/org/asteroidos/settings/always-on-display=false`).
- **mce logging:** `warning`.
- **Kernel:** trial build 004 (`#4-a8a8df5`) from `recovery`; `boot` holds the 2.1 release kernel.
