# Touch driver fix

The touchscreen could die until the next reboot: the display kept working, but no tap or swipe registered. The cause was a race in the Zinitix BT541 touch driver in Mobvoi's kernel. AsteroidOS rubyfish issue [#295](https://github.com/AsteroidOS/meta-smartwatch/issues/295) ("freezes after 2 or 3 minutes") is probably the same bug.

Status (2026-10-30):

- **Fixed** on this branch by six commits in `drivers/input/touchscreen/zinitix/zinitix.c`, listed [below](#the-fix). Trial build 010 (`#10-37a1cb9`, brightness fix included) runs from `recovery`.
- **Reproduced first**, on the unfixed driver: with the always-on display, off the cable, and the race forced, touch died 9 s after the watch started suspending, exactly as on 2026-10-29. The same test on the fixed driver ran 11 minutes without a single I2C error: the race came up 546 times and was handled every time.
- **The recovery code was tested on its own** with fault injection in test builds that are never committed: power-cycle, retries and backoff, then real taps after a forced recovery.
- **The always-on face wakes on a touch and hold, not a tap**, on the old kernel too: see [below](#waking-the-always-on-face-hold-dont-tap).
- **Flash to `boot`:** later, together with the other fixes, after the combined kernel has run from `recovery` for a day.

## What happened

Reproduced on 2026-10-30 (test build 012, the unfixed driver), from the kernel log (`dmesg`, kernel time):

1. With the always-on display, mce puts the panel in LP2 (`FB_BLANK_VSYNC_SUSPEND`) rather than off. The driver suspends the chip only on `FB_BLANK_POWERDOWN` or `FB_BLANK_NORMAL`, so the chip stays active while the system suspends, its interrupt armed as a wakeup source.
2. **76.450 s:** `PM: late suspend of devices complete`. A touch interrupt has just run the IRQ thread, which reads the chip over I2C. The I2C controller (`78b8000.i2c`, the chip's parent) has already disabled runtime PM (`Runtime PM-callback was not invoked.`). Then `suspend_device_irqs()` disables the controller's own interrupt and waits for the touch thread, so the transfer can't complete: `TIMEOUT_ERROR(val:510msec)` at 76.959 s.
3. The thread decides the chip is dead: `read fail`, then `bt541_power_control(POWER_ON_SEQUENCE)`, which resets the chip, then the vendor power sequence and `mini_init_touch`. All of it runs over the same stalled bus: four more timeouts in the power sequence (`Failed to send power sequence`) and one in mini init (`Failed to initialize mini init`), until 80.49 s. The suspend stalls for 3.5 s meanwhile.
4. The reset stopped the chip's firmware, and nothing restarts it. The chip still answers on I2C but returns junk (its firmware version reads `V1212113 HWID:14` instead of `V080123 HWID:0`), and it never raises its interrupt again. Touch is dead until a reboot, whose probe resets and restarts it.

The window is short: the thread must be running when the controller's interrupt gets disabled. A tap on the always-on face, which wakes the watch and lets it go back to sleep while the finger is still down, hits it from time to time. So do Wi-Fi wake storms (thousands of suspend/resume cycles per hour). With a forced interrupt every 200 ms (below), it took 9 s.

## Driver bugs

Source: `drivers/input/touchscreen/zinitix/zinitix.c`, Mobvoi `c428ef36`. The first five were listed here before the fix; the sixth and the rest were found while fixing and testing.

1. **No system-sleep handling.** No `dev_pm_ops`: the IRQ thread could use I2C at any point of a system suspend or resume.
2. **Recovery can't recover.** A failed read ran one `POWER_ON_SEQUENCE` and one `mini_init_touch`, in the IRQ thread, and gave up. On rover both regulators (`vdd`, `vcc_i2c`) are dummies (`4-0020 supply vdd not found, using dummy regulator`), so nothing was ever switched off; the only power switch is GPIO 46 (`gpio_switch`), which was left alone. A failed write of the clear-interrupt command was ignored too, although the chip keeps its interrupt line low until that command and the interrupt is edge-triggered.
3. **Inverted check in `bt541_ts_resume`.** `mini_init_touch()` returns `bool`, but resume compared it with `I2C_SUCCESS` (0): every display wake logged `resume_reset: zinitix_resume_proc err` (43 times in one night), and a real failure went unnoticed.
4. **`remove()` and probe's error paths leaked or used freed memory.** Besides GPIO 46 (rebind failed with `-EBUSY`), the fb notifier stayed registered inside the freed driver data, the `tsp` class and device were never destroyed (a rebind that got past the GPIO would have passed an `ERR_PTR` to `sysfs_create_link()` and oopsed), the input device was freed twice, and the resume work was flushed after `kfree()`.
5. **`enable_wakeup` mismatch.** Suspend and resume each read `/sys/devices/virtual/tsp/tsp/enable_wakeup`; if it changed in between, resume could skip powering the chip on. The power-on path also enabled the interrupt without disabling it, and GPIO errors returned with `work_lock` held.
6. **Display blank right after unblank.** Unblank queues the chip's resume as work, blank suspends at once: a blank before the work ran left the chip active with the display off.

## The fix

| Commit | What |
|---|---|
| `d17058debd80` | fix the inverted `mini_init_touch()` check in resume (bug 3) |
| `8618b7484963` | keep the IRQ thread off the bus during system suspend (bug 1) |
| `a67b4be2a336` | resume according to what suspend did (bug 5) |
| `aa65f4ea9ff9` | finish a queued resume before suspending (bug 6) |
| `6f312707c4ca` | power-cycle and retry a chip that stops answering (bug 2) |
| `37a1cb976cd7` | release everything on remove and probe failure (bug 4) |

- **System sleep.** New `.suspend`/`.resume` callbacks. From `.suspend` to `.resume` the IRQ thread doesn't touch the bus: it records the interrupt, reports a wakeup event (the device is now wakeup-capable, `power/wakeup`), aborts the suspend with `pm_system_wakeup()` as an armed wakeup interrupt would, and returns. The chip keeps the event. `.resume` runs after the I2C controller has resumed, logs `handling the interrupt deferred during suspend` and runs the handler. The interrupt stays a wakeup source, so a touch still wakes the watch, and a touch and hold still wakes the always-on display (see [below](#waking-the-always-on-face-hold-dont-tap)). Android touch drivers on Qualcomm commonly solve the same race by waiting in the IRQ thread for a "resume done" completion (Samsung's `sec_ts`, for one: "waiting for blsp block resuming, if not occurs i2c error"); deferring instead of waiting avoids blocking `suspend_device_irqs()`, which waits for the thread.
- **Recovery.** A failed read or clear is retried once. Then the interrupt is disabled, fingers that are down are released, and a freezable work (it never runs during a system suspend or resume) switches the chip off with GPIO 46 for 200 ms while holding it in reset, switches it on, runs the vendor reset and power sequence, writes its registers as resume does, clears the gesture register, and reads the firmware version back to be sure it runs. With the display off it goes back to idle mode. Failed attempts are retried after 100 ms, 1 s, 5 s, then every 30 s; the quick ones keep the system awake. A chip that fails again within a minute of a recovery continues the backoff, so it can't keep the watch awake. A failed display resume also starts it; a successful one ends it.
- **Remove and probe.** Stop the IRQ thread and the works first, then undo what probe did, in reverse.
- The patch is 400 lines in one file. Config, device trees and all 9046 exported symbol CRCs are unchanged.
- An independent review of the patch found no deadlock, IRQ imbalance or use-after-free; its findings on recovery coverage (the clear-interrupt write, a stale backoff timer, teardown order) are in the commits above.

## Tests (2026-10-30)

All from `recovery`, off the USB cable, Wi-Fi on, always-on display on unless said otherwise. A boot that starts on the cable never suspends (see `sonic/BUILD.md`, Gotchas), so every boot started off the cable. The second half of the 11-minute forced-race run was on a charger without USB data, where the watch suspends as usual.

**Forcing the race.** Waiting for a lucky tap is slow, so test builds have a knob that makes the chip raise its interrupt every N scan frames (register `0x00f1`, the vendor's ESD check): `periodic_frames=25` gives one every ~200 ms, and the IRQ thread runs across hundreds of suspend entries without anyone touching the watch. Fault injection makes every I2C transfer fail for N ms (`fault_ms`) or the next N coordinate reads fail (`fault_reads`), and `kick` runs the handler as an interrupt would. The knobs are in `build/kernel/0NN-*/stimulus.diff` and `test-knobs.diff` on the Mac; **never commit or ship them.**

| Test | Unfixed driver (build 012) | Fixed driver |
|---|---|---|
| Forced race, always-on, 200 ms interrupts | **touch dead** after 9 s of suspends: 6 I2C timeouts, `read fail`, power sequence and mini init failed; interrupts stopped for good | 11 min (test build 013): 715 suspends, 1068 wakes by touch, 3154 interrupts; the race came up 546 times, each handled at resume; no I2C error, no recovery |
| Real taps on the always-on face, 11 min (build 010) | (the reproduction above) | 2031 interrupts, 408 suspends, 52 wakes by touch; the race hit 30 times, each handled at resume; no I2C error, no recovery |

Recovery, with fault injection (test builds 011 and 013, fixed driver plus knobs):

| Case | Result |
|---|---|
| One failed read | retried, no recovery |
| Two failed reads | recovery attempt 1: GPIO 46 off and on, reset held, power sequence OK (`chip code = 0xe628`), firmware version read back, interrupt serviced again 0.76 s after the failure |
| Bus dead for 12 s, always-on face | attempts at 0, +100 ms, +1 s, +5 s fail; the one at +30 s succeeds; ambient redraws don't disturb the backoff |
| Display wake with the bus dead for 2 s | `resume failed, recovering the controller`; attempt 2 succeeds |
| Display off (tap-to-wake idle mode), bus dead 3 s | recovery succeeds on attempt 3 and puts the chip back in idle mode |
| Real taps after a forced recovery | 3 taps and a swipe all worked (179 interrupts, 270 input events) |

Regressions, on build 010 unless said otherwise:

- `trial-check.sh`: healthy on every boot. The kernel log differs from the normal reference boot only by version strings and the environment (Wi-Fi, IRQ affinity, systemd timing); nothing from the touch driver beyond its usual lines.
- Screen off and on with the button and the always-on display **off**, so the chip goes through its full display suspend (tap-to-wake idle mode) and resume: 4 cycles by hand and 3 emulated (`mcetool --powerkey-event=short`). Touch worked after every wake, and `resume_reset: zinitix_resume_proc err` no longer appears.
- The same with the always-on display **on** (on ↔ ambient): 3 emulated cycles; the chip's firmware version read back after each.
- Waking the always-on face by touch and hold: works, as before.
- **Bug 4**, one unbind and rebind of the driver (test build 013, whose `remove()` and probe are identical to 010's): the unbind released GPIO 46 and GPIO 44 (`pinmux-pins`: `GPIO UNCLAIMED`), the `tsp` class, the interrupt, the wakeup source, the input and misc devices. The rebind succeeded (`bt541_ts_probe: SUCCESS`, firmware `V080123`), re-acquired all of them, and the rebound driver then handled 108 forced interrupts, 23 of them across a suspend. No warning. After a rebind, the launcher doesn't reopen the touch device, so the UI needs a restart or a reboot.

## Build and test

- **Build and trial-boot** exactly as in `sonic/BUILD.md`. Boot off the cable, and keep the watch in Wi-Fi range while it boots. Its dead-man's switch now counts boot time: with `sleep 600` an unconfirmed trial off the cable was still running 13 minutes after boot.
- **The test knobs** live outside the repository; to force the race again, apply `stimulus.diff` to this driver, build with `kbuild.sh --worktree`, boot it off the cable with always-on on, then `echo 25 > /sys/module/zinitix/parameters/periodic_frames`.
- **Upstream:** worth sending, since AsteroidOS's always-on display hits it: an issue and a patch series for AsteroidOS/meta-smartwatch (`meta-rubyfish/recipes-kernel/linux/`), mentioning issue #295.

## Not fixed here

Found while fixing, left as they were because AsteroidOS doesn't reach them:

- The calibration ioctl of the misc device (`TOUCH_IOCTL_HW_CALIBRAION`) returns without re-enabling the interrupt when the driver is busy, which leaves touch off. Nothing on AsteroidOS opens `/dev/zinitix_touch_misc`.
- `remove()` can race an ioctl that is blocked on `work_lock`.
- `finger_cnt1` (a `u8`) can wrap below zero on a release that was never reported as a press; `BTN_TOUCH` then stays down until the next screen-off. Multi-touch slots, which Qt uses, are unaffected. Not seen in the tests: every captured tap had a matching press and release.
- The fb notifier reacts to every framebuffer, `fb1` included, so the chip is suspended twice per screen-off (`bt541_ts_suspend: already disabled`). Harmless.
- With the always-on display off, a tap on the dark screen makes the driver report `KEY_WAKEUP`, but mce ignores it: use the button. The same on the old kernel.

## Waking the always-on face: hold, don't tap

AsteroidOS 2.1 wakes the dim always-on face on a **touch and hold**, not a tap. mce turns a press of about 0.2 s into its "double tap" gesture (`[longpress] double tap emulated from touch input`, then `gesture(4)` and `display state req: ON` in its log). A short tap (40–60 ms) reaches userspace intact but wakes nothing, on the old kernel as on the fixed one. Measured 2026-10-30 from the raw events of `/dev/input/event3`: ten short taps, no wake; every hold woke it.

## If touch dies anyway

- The kernel log says why: `journalctl -k | grep -E 'zinitix|78b8000'`. Recovery logs `recovering the controller`, `recovery attempt N failed` and `controller recovered`.
- Pressing the button to wake the display retries a pending recovery right away, if the display was fully off.
- A reboot still restarts the chip.
