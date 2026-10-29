# Brightness fix (to do)

Make automatic brightness work on the watch:

- Fix a kernel bug that holds back brightness changes.
- Then retune the brightness curve to suit me.

Status (2026-10-29): diagnosed, nothing changed yet. Kernel rebuild deferred.

## Symptoms

- Writing to `/sys/class/leds/lcd-backlight/brightness` seems to do nothing.
- The Settings brightness slider seems to jump between only two levels, "bright" and "dimmer".
- Automatic brightness looks erratic and generally too dim.

## Diagnosis

The panel and the brightness path are fine. **The kernel holds back brightness changes until the screen redraws.**

- **Panel:** Chipone **ICNA3310** AMOLED, command mode (`msm_fb_panel_info`).
- **Brightness command:** DCS `0x51 <level>`, one byte, range 1–255 (`qcom,mdss-dsi-bl-pmic-control-type = bl_ctrl_dcs`). The panel's start-up commands enable it (`fe 00`, `51 ff`, `53 20`), so the panel accepts it.
- **Proof that changes are held back:** `/sys/class/graphics/fb0/bl_event` counts brightness updates actually sent to the panel. On a static screen it does not change after a write:
  ```
  ssh sonic-screwdriver 'E=/sys/class/graphics/fb0/bl_event; B=/sys/class/leds/lcd-backlight/brightness
    c0=$(cat $E); echo 150 > $B; c1=$(cat $E); echo "$c0 -> $c1"'
  # 2026-10-29: 6014 -> 6014  (held back)
  ```
- **Visual check:** stepping 255 → 64 → 16 → 4 *while scrolling a list*, so the screen redraws constantly, gives clearly different brightness levels. On a static screen it does nothing.

### Root cause in the kernel

- **Source:** `mobvoi/mobvoi-ticwatch-kernel`, branch `mobvoi-android-msm-rover-4.9`, commit `c428ef3654d52e816308a6cf11009a1742f86c1c`. AsteroidOS's `meta-rubyfish` adds patches 0001–0006, none of which touch the display.
- **File:** `drivers/video/fbdev/msm/mdss_fb.c`.

How the block is supposed to work:

1. **On unblank** (`mdss_fb_blank_sub`, around line 2147), the driver sets `allow_bl_update = false`, so the backlight doesn't come on before the first frame reaches the panel.
2. **While blocked**, `mdss_fb_set_backlight()` only stores the requested value in `unset_bl_level` and returns.
3. **After each frame** (the `skip_commit:` path, around line 4043), `mdss_fb_update_backlight()` applies any held-back value and sets `allow_bl_update = true`.

The bug is that `mdss_fb_update_backlight()` returns early when nothing is pending (`unset_bl_level == U32_MAX`), *without* lifting the block. If mce's first write after a wake arrives after the first frame, the block stays on. Every write then waits for the next redraw, and a watch face only redraws about once a minute.

### Proposed patch (untested)

Lift the block at the first frame after unblank even when nothing is pending:

```diff
--- a/drivers/video/fbdev/msm/mdss_fb.c
+++ b/drivers/video/fbdev/msm/mdss_fb.c
@@ void mdss_fb_update_backlight(struct msm_fb_data_type *mfd)
-	if (mfd->unset_bl_level == U32_MAX)
-		return;
+	if (mfd->unset_bl_level == U32_MAX) {
+		/*
+		 * Nothing pending, but a frame has now reached the panel since
+		 * unblank, so stop holding back later brightness writes.
+		 */
+		mutex_lock(&mfd->bl_lock);
+		mfd->allow_bl_update = true;
+		mutex_unlock(&mfd->bl_lock);
+		return;
+	}
```

Before building, check the other callers of `mdss_fb_update_backlight()` outside `mdss_fb.c`, such as `mdss_mdp_overlay.c`, to make sure each one runs only after a frame has gone out.

## Plan: build, test, flash

What I know so far:

- **Docker** works on the Mac. The watch's kernel is **32-bit ARM** (`CONFIG_ARM=y`, `armv7l`), so the build is a cross-compile (`ARCH=arm`, an `arm-linux-gnueabihf-` toolchain) inside a Linux container.
- **The watch's exact kernel config** is available from `/proc/config.gz`.
- **No kernel modules are loaded**, though `/lib/modules/4.9.232` exists.
- **Running kernel:** `4.9.232 #1 SMP PREEMPT Wed Nov 3 10:42:30 UTC 2021`. That is `c428ef36`'s commit time to the second (the build date comes from the source commit), so the running kernel was built from it.

Steps:

1. **Sources:** this branch. It starts from `asteroidos-2.1`, which is `c428ef36` plus meta-rubyfish's patches 0001–0005 from `meta-smartwatch`'s `2.0` branch (what AsteroidOS **2.1** is built from) and the watch's running config as `arch/arm/configs/asteroidos-rubyfish_defconfig`.
   - `0006-net-bcmhd-Allow-compilation-as-module.patch` is left out on purpose. It came after 2.1 and turns Wi-Fi into a module that the 2.1 system doesn't expect.
   - Check it out on a case-sensitive filesystem, such as a Docker volume. On macOS's default case-insensitive filesystem, 12 pairs of netfilter files whose names differ only in case (`xt_DSCP.c` and `xt_dscp.c`, …) overwrite each other and break the build.
2. **Add the fix** as a commit on this branch. For meta-rubyfish it becomes `0007-…patch`.
3. **Configure:** `make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- asteroidos-rubyfish_defconfig`. That file is the watch's `/proc/config.gz`, so diff it against the watch's before the first build. For sound, also restore stock's tracing settings (see [sound.md](sound.md), on the `sound` branch).
4. **Build** the same image type as the release, with the same appended device trees for rubyfish and rover. Check `linux-rubyfish_p.bb` for `KERNEL_IMAGETYPE` and any custom deploy step. The release ships it as `zImage-dtb-rubyfish.fastboot`.
5. **Repack the boot image.** Unpack the 2.1 one (`~/Downloads/asteroidos-rubyfish-2.1/asteroid-rubyfish-boot.img`) with `unpack_bootimg`. Then rebuild it with `mkbootimg`, using the new kernel and the same ramdisk, command line, base, page size and offsets.
6. **Test without touching the installed kernel. No USB is needed.** Write the test image to the **recovery** partition over SSH and boot it once. The 2.1 kernel stays on `boot`.
   ```
   scp new-boot.img sonic-screwdriver:/tmp/
   ssh sonic-screwdriver 'dd if=/tmp/new-boot.img of=/dev/block/platform/msm_sdcc.1/by-name/recovery && sync && systemctl reboot recovery'
   ```
   - Before rebooting, read the partition back and compare checksums. The image must fit the 32 MiB partition; the 2.1 one is about 13 MB.
   - If the reboot argument isn't honoured, run `ssh sonic-screwdriver reboot bootloader` and choose Recovery mode in the fastboot menu (bottom button cycles, top button selects).
   - If the test kernel hangs, hold the top button for about 8 s. The PMIC forces a restart that boots the 2.1 kernel from `boot`. The persistent journal keeps the failed boot's logs.
   - With the watch on the USB dock, `fastboot boot new-boot.img` is an alternative, but a 2023 porter reported this bootloader may not support it.

   **Making sure Wi-Fi comes up, or that the watch comes back without it:**
   - **Match the running kernel.** Use the same source, the same 2.1 patches and the config from `/proc/config.gz`. The Wi-Fi driver is built in, its firmware and NVRAM are on `/vendor`, and its MAC is in `/persist`, so a matching build brings Wi-Fi up as today.
   - **Check on the Mac before writing anything to the watch:**
     - the config diff contains only the intended changes;
     - the appended device tree is byte-identical to the running one (`/sys/firmware/fdt` on the watch);
     - the command line and ramdisk are identical to the 2.1 boot image's.
   - **Baseline trial first:** the first test kernel has no fixes. If it boots with Wi-Fi, the build pipeline is proven, and a later failure points at a patch.
   - **Trial command line:** append `sonic.trial=1 msm_poweroff.download_mode=0`. Download mode is **on** today (`/sys/module/msm_poweroff/parameters/download_mode` = 1). With it on, a panic parks the watch in Qualcomm's USB dump mode instead of rebooting. With it off, the existing 5 s panic timeout (`CONFIG_PANIC_TIMEOUT=5`, panic on oops) reboots into the normal kernel.
   - **Automatic rollback ("dead man's switch").** Install this once on the watch. It does nothing on normal boots:
     ```
     # /etc/systemd/system/kernel-trial.service  (systemctl enable kernel-trial)
     [Unit]
     Description=Return to the normal kernel unless this trial kernel is confirmed
     ConditionKernelCommandLine=sonic.trial=1
     [Service]
     ExecStart=/bin/sh -c 'sleep 600; [ -e /run/sonic-trial-ok ] || systemctl reboot'
     [Install]
     WantedBy=multi-user.target
     ```
     Once the trial looks good over SSH, `touch /run/sonic-trial-ok` keeps it running. Otherwise, after 10 minutes it reboots into the normal kernel on its own. The trial only boots from recovery once, so that reboot lands on `boot`.
   - **Remaining case:** a hang that never panics and never reaches userspace still needs the top button held for about 8 s.
   - Check that `uname -v` shows the new build.
   - Blank and unblank (`mcetool --blank-screen; mcetool --unblank-screen`), then run the `bl_event` check above. The counter must now go up on every write.
   - Look at the screen: brightness should follow writes and the slider immediately.
7. **Flash** only once it works. Write it to `boot` the same way, over SSH (or with `fastboot flash boot`).
   - Keep the known-good 2.1 kernel in `recovery`, so the button-driven fastboot menu can always reach a working kernel. USB is then only needed if both partitions end up broken.
   - Rollback is the original 2.1 boot image above.
   - The full stock backup is in `~/Documents/sonic-screwdriver/stock-backup/`.
8. **Upstream:** open an issue on AsteroidOS/meta-smartwatch (label `rubyfish`) with this diagnosis, and a PR adding the patch to `meta-rubyfish/recipes-kernel/linux/`.

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

When the screen wakes, mce treats "no reading yet" as darkness and starts at the darkest step until the first lux sample arrives. If that shows as a dim flash on wake, look at mce's `als` handling. Otherwise ignore it.

## Current watch state (2026-10-29)

- **Automatic brightness:** off (`als_enabled=false`; the `als_autobrightness` flag is still on).
- **Brightness setting:** 50%.
- **mce logging:** back to `warning`.
- **Screen timeout:** normal (no stay-on inhibit).
