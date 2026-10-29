# Touch driver fix (to do)

The touchscreen can die until the next reboot. The display keeps working, but no tap or swipe registers. The cause is a race in the Zinitix BT541 touch driver in Mobvoi's kernel. AsteroidOS rubyfish issue [#295](https://github.com/AsteroidOS/meta-smartwatch/issues/295) ("freezes after 2 or 3 minutes") is probably the same bug.

Status (2026-10-29): root cause found from logs and source. Not fixed yet; it needs a kernel rebuild.

## What happened (boot of 2026-10-29 19:30–19:56)

Timestamps are from that boot's journal, `journalctl -b <id> -o short-monotonic`.

1. **655.5 s:** mce sends the display to **LPM_ON** (low-power mode) rather than off. The touch driver only suspends on `FB_BLANK_POWERDOWN`, so the chip stays fully active, with its interrupt armed as a wake-up source.
2. **663.3 s:** the system enters deep sleep, and a touch interrupt wakes it (`pm_system_irq_wakeup: 428 triggered bt541_ts_device`).
3. **671.50 s:** the system is going to sleep again; `PM: late suspend of devices complete` has been logged. The touch IRQ thread is still running and reads over I2C, but the bus controller (`78b8000.i2c`) is already suspended and runtime PM is off. Every transfer times out:
   ```
   i2c-msm-v2 78b8000.i2c: TIMEOUT_ERROR(val:510msec) misconfigured GPIO or slave pulling bus line(s) low
   i2c-msm-v2 78b8000.i2c: Runtime PM-callback was not invoked.
   ```
4. **671.51–671.59 s:** the driver decides the chip is dead and goes through `ts_read_coord` failed, then `bt541_power_control(POWER_ON_SEQUENCE)`, then `mini_init_touch`. All of that runs over the same suspended bus, so it fails: `Failed to send power sequence`, `Failed to initialize mini init`.
5. Nothing ever retries, so touch stays dead after resume. The only device on bus 4 is the touch chip (`4-0020`).

## Driver bugs

Source: `drivers/input/touchscreen/zinitix/zinitix.c` in `mobvoi/mobvoi-ticwatch-kernel`, branch `mobvoi-android-msm-rover-4.9`, commit `c428ef36`.

1. **No system-sleep handling.** There are no `dev_pm_ops`. Suspend and resume only follow display blank events (`fb_notifier_callback`), so the IRQ thread can use I2C while the system suspends or resumes. With `USE_WAKEUP_GESTURE 1` the IRQ is a wake source, and AsteroidOS's LPM display keeps the chip active.
2. **Recovery can't recover.** `bt541_touch_work` handles a failed read with one `POWER_ON_SEQUENCE` and one `mini_init_touch`, then gives up for good:
   - `POWER_ON_SEQUENCE` calls `regulator_enable` again, which is reference-counted, so nothing actually power-cycles.
   - It never toggles the power-switch GPIO 46 (`gpio_switch`).
3. **Inverted check in `bt541_ts_resume`.** It tests `mini_init_touch(info) != I2C_SUCCESS`, but `mini_init_touch` returns `bool` (true on success). So it logs `resume_reset: zinitix_resume_proc err` after a *successful* resume, and skips the code after it.
4. **`bt541_ts_remove` leaks the switch GPIO.** It never frees `gpio_switch` (46), so the driver can't be rebound (`unable to request switch gpio 46`, probe error -16).
5. **`enable_wakeup` mismatch.** If `/sys/devices/virtual/tsp/tsp/enable_wakeup` changes between suspend and resume, resume can skip switching the chip's power back on.

## Proposed patch

1. **System PM ops, the usual pattern in Android touch drivers:**
   - `.suspend`: set `pm_suspended`, `reinit_completion(&pm_done)`, then `synchronize_irq()`. The IRQ stays wake-capable.
   - `.resume`: clear `pm_suspended` and `complete_all(&pm_done)`. The I2C controller is the touch device's parent, so it has already resumed by then.
   - IRQ thread: if `pm_suspended`, call `pm_wakeup_event()`, then `wait_for_completion_timeout(&pm_done, ~1 s)` before any I2C access. On timeout, clear the interrupt later and return.
2. **Real, retried recovery.** Replace the in-IRQ reset with delayed recovery work that:
   - waits until the system is resumed;
   - power-cycles for real: `gpio_switch` off, regulators off, a delay, then on, then reset, power sequence and `init_touch`;
   - retries with backoff (100 ms, 1 s, 5 s, 30 s, …) instead of giving up;
   - first retries the failed read once, so a single glitch doesn't trigger a reset.
3. **Fix the resume check** to `if (!mini_init_touch(info))`.
4. **Free `gpio_switch`** in `remove()` and in the probe error paths.

## Plan

Build it together with the backlight fix in [brightness_fix.md](brightness_fix.md), on the `brightness-fix` branch: same kernel, same build, test and flash steps. Test before flashing:

1. Boot the patched kernel without touching the installed one: `fastboot boot`, or the recovery-partition fallback described in [brightness_fix.md](brightness_fix.md).
2. Run the stress test:
   - let the display drop to LPM and the system suspend, and keep tapping across the sleep transitions for several minutes;
   - watch `dmesg` for the new recovery messages, and check that touch keeps working;
   - repeat with Wi-Fi on and off, since Wi-Fi also wakes the system often.
3. Only then write it to `boot`, and keep the known-good 2.1 kernel in `recovery` as the fallback, as described in [brightness_fix.md](brightness_fix.md).
4. Send upstream: AsteroidOS meta-smartwatch (rubyfish kernel patch) and issue #295.

## If touch dies before the fix

- **Reboot.** That's the only sure recovery today: `ssh sonic-screwdriver reboot`.
- **Don't unbind or rebind the driver.** Bug 4 above makes the rebind fail and leaves no touch device at all.
- **Unverified:** a full display off and on, for example with the power button, takes the driver through suspend and resume, which resets and re-initialises the chip. It might recover touch without a reboot.
