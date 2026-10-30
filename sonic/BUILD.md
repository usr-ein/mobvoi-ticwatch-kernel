# Building and trial-booting kernels for Sonic Screwdriver

Sonic Screwdriver is a TicWatch Pro 3 Ultra (codename `rover`) running
AsteroidOS 2.1 (the `rubyfish` image). This directory holds everything needed
to build a branch of this kernel the way AsteroidOS builds it, prove what
differs from the kernel the watch runs, and boot it **once** from the watch's
`recovery` partition, with automatic fallback to the normal kernel.

Branch `asteroidos-2.1` is the baseline: Mobvoi's `c428ef36`, AsteroidOS's
meta-rubyfish patches 0001–0005 (meta-smartwatch `2.0` @ `7d6de21fbe11`), and
`arch/arm/configs/asteroidos-rubyfish_defconfig` (= the watch's
`/proc/config.gz`). Fix branches (`brightness-fix`, `touch-driver-fix`,
`sound`, …) start from it.

Everything below is copy-pasteable from a Mac shell (zsh or bash). Paths are
the ones used on the user's Mac; override them with the environment variables
listed under [Files and variables](#files-and-variables).

## Safety rules

Read these before touching the watch.

- **Only `recovery` is ever written.** Never write `boot` or any other
  partition. `boot` holds the AsteroidOS 2.1 release kernel and is what the
  watch falls back to. `flash-recovery.sh` refuses to run if `boot` does not
  hold it.
- **Allowed writes on the watch:** the `recovery` partition,
  `/etc/systemd/system/kernel-trial.service` and
  `/etc/systemd/system/kernel-trial-wifi.service` with their enable symlinks,
  `/run/sonic-trial-ok`, temporary files in `/tmp`. Nothing else.
- **Every trial image carries `sonic.trial=1 msm_poweroff.download_mode=0`.**
  The first arms the dead-man's switch; the second makes a panic reboot (after
  5 s, `CONFIG_PANIC_TIMEOUT=5`, `panic_on_oops=1`) instead of parking the
  watch in Qualcomm's USB download mode. `kbuild.sh` produces such an image as
  `boot-trial.img`; `flash-recovery.sh` refuses images without them.
- **Never flash a `--repro` build.** Its version string is the release's, so a
  trial would be indistinguishable from the normal kernel. `flash-recovery.sh`
  refuses it.
- Never unbind or rebind drivers, the touch driver above all: its `remove()`
  leaks GPIO 46, a rebind fails, and the touchscreen is gone until a reboot.
- Never read `/sys/power/wakeup_count`. Don't change Wi-Fi, Bluetooth or other
  settings, and never copy Wi-Fi credentials anywhere.
- Avoid sysfs files with side effects: e.g. reading
  `/sys/class/graphics/fb0/msm_fb_panel_status` sends DSI commands to the panel.
- **Private data stays on the Mac.** `/proc/cmdline`, kernel logs and
  `bluetoothctl` output contain the serial number, MAC addresses, partition
  UUIDs, device names and SSIDs. Keep them in
  `~/Documents/sonic-screwdriver/build/kernel/` (git-ignored); never commit or
  push them. This repository is public.
- If the watch needs a button press, stop and tell the user. Don't improvise
  with `fastboot`: never `fastboot flash`/`erase`/`boot`/`oem`/`flashing`.
- Never check this tree out on the Mac: its APFS is case-insensitive and 12
  netfilter file pairs differ only in case (`xt_DSCP.c`/`xt_dscp.c`, …). The
  working tree lives in a Docker volume.

## How a trial works, and how it falls back

1. `flash-recovery.sh` writes `boot-trial.img` to `recovery` over SSH and reads
   it back.
2. `systemctl reboot --reboot-argument=recovery` boots `recovery` once. The
   bootloader goes back to `boot` on the next reboot by itself.
3. `kernel-trial.service` (the dead-man's switch, below) starts only when the
   cmdline has `sonic.trial=1`. It waits until the watch has been up for 10
   minutes, suspend included, then reboots unless `/run/sonic-trial-ok` exists.
   `trial-check.sh --confirm` creates that file once the trial looks healthy.

| What goes wrong | What happens |
|---|---|
| Trial kernel panics or oopses | reboots after 5 s into the normal kernel on `boot` |
| Boots, but you don't confirm within 10 min | dead-man's switch reboots into `boot` |
| Boots but SSH/Wi-Fi is dead | wait 10 min; meanwhile `adb shell` works over the USB cable if it's plugged in. If Wi-Fi failed to connect at boot, `kernel-trial-wifi.service` (below) retries for 2 min; connman can also be told to reconnect by hand (see Gotchas) |
| Hangs before userspace, no panic | needs the user: hold the **top** button ~8 s (PMIC reset), the watch boots `boot` |

The persistent journal keeps the failed boot: `journalctl --list-boots`, then
`journalctl -k -b -1`. It only holds about 10 hours (see Gotchas), so read it
soon.

The dead-man's switch, installed on the watch on 2026-10-30 (identical to
`sonic/kernel-trial.service`):

```
# /etc/systemd/system/kernel-trial.service  (systemctl enable kernel-trial)
[Unit]
Description=Return to the normal kernel unless this trial kernel is confirmed
ConditionKernelCommandLine=sonic.trial=1
[Service]
# Wait for 600 s of boot time, suspend included (/proc/uptime): a plain
# `sleep 600` counts only the time the watch is awake, so off the cable it
# can take far longer. The check runs at the first wakeup past 600 s.
ExecStart=/bin/sh -c 'while [ "$$(cut -d. -f1 /proc/uptime)" -lt 600 ]; do sleep 5; done; [ -e /run/sonic-trial-ok ] || systemctl reboot'
[Install]
WantedBy=multi-user.target
```

It counts boot time because `sleep` doesn't count suspend: the first
version, `sleep 600`, fired after exactly 600 s on the cable, where the watch
never suspends, but an unconfirmed trial off the cable was still running 13
minutes after boot (2026-10-30). The check itself still needs the watch to be
awake; it wakes every few seconds to minutes on its own (Wi-Fi, timers).
`$$` is how a unit file passes a literal `$` to the shell.

Check it is still there: `ssh sonic-screwdriver systemctl is-enabled kernel-trial`
(must print `enabled`). To reinstall:

```sh
ssh sonic-screwdriver 'cat > /etc/systemd/system/kernel-trial.service && systemctl daemon-reload && systemctl enable kernel-trial' < "$T/sonic/kernel-trial.service"
```

The Wi-Fi retry, installed on 2026-10-30 (identical to
`sonic/kernel-trial-wifi.service`). connman doesn't retry a failed first
association (see Gotchas), and a trial that can't be reached can't be
confirmed either, so the dead-man's switch takes it back. Like the switch, it
runs only on trial boots:

```
# /etc/systemd/system/kernel-trial-wifi.service  (systemctl enable kernel-trial-wifi)
[Unit]
Description=Reconnect the saved Wi-Fi network if a trial kernel boots without it
ConditionKernelCommandLine=sonic.trial=1
After=connman.service
[Service]
# connman doesn't retry a failed first association (BUILD.md, Gotchas), and a
# trial that can't be reached can't be confirmed. From 20 s after the start,
# for 2 minutes: while connman is neither ready nor online, scan, then connect
# the saved network (the first service `connmanctl services` marks `*A`:
# saved, autoconnect). Changes no setting and reads no credentials.
ExecStart=/bin/sh -c "sleep 20; n=0; while [ $$n -lt 8 ]; do connmanctl state | grep -q -E 'State = (ready|online)' && exit 0; connmanctl scan wifi; for s in $$(connmanctl services | awk '/^[*]A/ {print $$NF; exit}'); do connmanctl connect $$s; done; sleep 15; n=$$((n + 1)); done"
[Install]
WantedBy=multi-user.target
```

Install it, check it (must print `enabled`), and undo it:

```sh
ssh sonic-screwdriver 'cat > /etc/systemd/system/kernel-trial-wifi.service && systemctl daemon-reload && systemctl enable kernel-trial-wifi' < "$T/sonic/kernel-trial-wifi.service"
ssh sonic-screwdriver systemctl is-enabled kernel-trial-wifi
ssh sonic-screwdriver 'systemctl disable kernel-trial-wifi && rm /etc/systemd/system/kernel-trial-wifi.service && systemctl daemon-reload'
```

`journalctl -b -u kernel-trial-wifi` shows what it did on a trial boot.

## One-time setup

1. Docker Desktop must be running (`docker info`; if not, `open -a Docker`).
2. The 2.1 release must be in `~/Downloads/asteroidos-rubyfish-2.1/`
   (`asteroid-rubyfish-boot.img`; the scripts check its SHA-512).
3. Extract these tools from the bare repo (a plain checkout is not possible,
   see Safety rules; `sonic/` itself has no case clashes). Do this again
   whenever `asteroidos-2.1` changes:

   ```sh
   T=~/Documents/sonic-screwdriver/build/kernel-tools
   rm -rf "$T" && mkdir -p "$T"
   git -C ~/Documents/mobvoi-ticwatch-kernel.git archive asteroidos-2.1 sonic | tar -x -C "$T"
   ```

   All later commands use `$T/sonic/…`. The tools always come from
   `asteroidos-2.1`, whatever branch you build.

The first build creates the Docker image `sonic-kernel-builder` (≈1 min) and
the volumes `sonic-kernel-work` (source tree, cloned from the bare repo, ≈5 GB
with build output) and `sonic-kernel-ccache` (≈0.2 GB per configuration).

## Making a change on a branch

The source tree is `/work/src` in the `sonic-kernel-work` volume, a clone of the
bare repo (its `origin`). `kshell.sh` opens a shell there, with the bare repo
mounted read-write at `/repo` and, optionally, a Mac directory at `/mac` for
exchanging files with your editor:

```sh
X=~/Documents/sonic-screwdriver/build/kernel/edit; mkdir -p "$X"
# 1. Check out the branch and copy the files you need to the Mac:
SONIC_MAC_DIR="$X" "$T/sonic/kshell.sh" '
  git fetch -q origin && git checkout -q -B brightness-fix origin/brightness-fix &&
  mkdir -p /mac/drivers/video/fbdev/msm &&
  cp drivers/video/fbdev/msm/mdss_fb.c /mac/drivers/video/fbdev/msm/'
# 2. Edit "$X/drivers/video/fbdev/msm/mdss_fb.c" on the Mac.
# 3. Copy back, commit, and push the branch to the bare repo:
SONIC_MAC_DIR="$X" "$T/sonic/kshell.sh" '
  cp /mac/drivers/video/fbdev/msm/mdss_fb.c drivers/video/fbdev/msm/ &&
  git diff --stat &&
  git -c user.name="Samuel Prevost" -c user.email=usr_ein@pm.me commit -q -a \
      -m "mdss: lift the backlight hold-back on the first frame after unblank" \
      -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" &&
  git push -q origin HEAD:brightness-fix && git log -1 --oneline'
```

`kbuild.sh` builds branches from the bare repo, so push before building (or
build uncommitted work with `--worktree`). Publishing to GitHub is a separate,
deliberate step: `git -C ~/Documents/mobvoi-ticwatch-kernel.git push origin brightness-fix`.

## Build

```sh
"$T/sonic/kbuild.sh" asteroidos-2.1          # or brightness-fix, touch-driver-fix, sound, a commit…
```

A full build takes about 2 minutes on the M-series Mac (25 s when ccache is
warm). Each build gets the next number and lands in
`~/Documents/sonic-screwdriver/build/kernel/NNN-BRANCH/`:

| File | What |
|---|---|
| `boot-trial.img` | **the image to test**: new kernel, release ramdisk/header, release cmdline + trial arguments |
| `boot.img` | same kernel with the plain release cmdline (for a later permanent install; not used in trials) |
| `zImage-dtb` | the kernel: zImage + rover.dtb + rubyfish.dtb |
| `config`, `System.map`, `Module.symvers`, `vmlinux`, `modules/` | build products (`vmlinux` has debug info, for decoding oopses) |
| `build-info.txt` | branch, commit, `uname -v`, compiler, SHA-256s |
| `build.log`, `verify.txt` | logs |

Each output directory takes ≈225 MB (mostly `vmlinux`); delete old ones when
they are no longer needed.

`uname -v` of every build is `#<build number>-<commit> SMP PREEMPT <UTC build time>`,
e.g. `#2-4fffe77 SMP PREEMPT Fri Oct 30 10:05:14 UTC 2026` (`-dirty` is
appended for `--worktree` builds of uncommitted changes), so a trial is never
mistaken for the installed kernel (`#1 SMP PREEMPT Wed Nov 3 10:42:30 UTC 2021`).

Options: `--repro` (release version strings, for the byte comparison only;
never flash), `--worktree` (build `/work/src` as it is), `--no-verify`,
`--allow-config-diff` (for branches that change the config on purpose, e.g.
`sound`).

### What the build replicates

AsteroidOS's recipe is `meta-rubyfish/recipes-kernel/linux/linux-rubyfish_p.bb`
(meta-smartwatch `2.0`), which pulls in meta-oe's `linux.inc`, oe-core's
`kernel.bbclass`/`kernel-arch.bbclass` (walnascar) and meta-asteroid's
`mkboot` and `old-kernel-gcc-hdrs` classes.

| AsteroidOS | Here |
|---|---|
| GCC 14.3.0 + binutils 2.44 (Yocto walnascar) | the same versions, vanilla: kernel.org crosstool `arm-linux-gnueabi-` (Dockerfile, SHA-256 pinned) |
| `KERNEL_IMAGETYPE = "zImage-dtb"` (meta-asteroid `hybris-watch.inc`) | `make zImage-dtb modules` |
| `.config`: defconfig + `kernel_conf_variable`s, `oldconfig`, `olddefconfig` | `make asteroidos-rubyfish_defconfig` (the file already is the result) |
| in-tree build (`B = S`), `ARCH=arm`, `LOCALVERSION=""` + empty `.scmversion` | in-tree, `ARCH=arm`, `LOCALVERSION=` (no `+` suffix) |
| `KERNEL_CC` adds `-fcanon-prefix-map` (and prefix maps for build dirs) | `CC="ccache arm-linux-gnueabi-gcc -fcanon-prefix-map"`: with the kernel's own `-fmacro-prefix-map=./=` it turns `__FILE__` into `/init/main.c`, as in the release |
| `LC_ALL=en_US.UTF-8`, `TZ=UTC` | the same (`nm -n` sorts ties with `strcoll()`, which changes kallsyms) |
| `KBUILD_BUILD_USER=oe-user`, `KBUILD_BUILD_HOST=oe-host`, `KCONFIG_NOTIMESTAMP=1` | the same |
| `KBUILD_BUILD_TIMESTAMP` = commit time of `c428ef36`, version `#1` | build time and build number (`--repro`: the release's) |
| DTBs appended in `find` order (`CONFIG_BUILD_ARM_APPENDED_DTB_IMAGE_NAMES=""`) | always `rover.dtb`, `rubyfish.dtb`: the release's order; `find` order depends on the filesystem |
| `old-kernel-gcc-hdrs`: dummy `include/linux/compiler-gcc{5..15}.h` | skipped: 4.9.232 only includes `compiler-gcc.h`, so they are never used |
| `mkboot` with `img_info`, xiaolu/mkbootimg_tools (AOSP mkbootimg of 2015) | `bootimg.py pack`, the same algorithm (Debian's current AOSP `mkbootimg` writes `second_addr` 0 instead of 0x80f00000) |
| ramdisk `initramfs-android-image-rubyfish.cpio.gz`, cmdline from `img_info` | the release boot image's own ramdisk and cmdline |
| host tools with `HOSTCFLAGS="-O2 -pipe"`, host GCC of the build machine | the kernel's default `HOSTCFLAGS`, Debian trixie GCC 14.2 (host tools don't end up in the image) |

## Verify (before anything touches the watch)

`kbuild.sh` runs `verify.py` after every build and stops on failure. It
compares the build with the 2.1 release boot image:

1. the packer rebuilds the release image byte for byte from its own parts;
2. the build's config equals the release kernel's embedded config (IKCONFIG,
   i.e. the watch's `/proc/config.gz`), header included, and the new kernel
   embeds the same; with `--allow-config-diff` it prints the diff instead;
3. both boot images keep the release's header values (addresses, page size,
   board name) and ramdisk byte for byte; `boot-trial.img`'s cmdline is the
   release's plus `sonic.trial=1 msm_poweroff.download_mode=0`; the appended
   DTBs are byte-identical to the release's, in the same order;
4. `boot-trial.img` fits the 32 MiB recovery partition;
5. the kernel says `4.9.232`; it also reports whether the decompressed kernel
   Image equals the release's, and
6. whether the exported symbol CRCs (MODVERSIONS) match the release's 9046:
   if they do, the three modules on the rootfs (`br_netfilter`, `rdbg`, `lcd`)
   still load and the module ABI is unchanged.

To re-run it on a build directory `$B`:
`python3 -I "$T/sonic/verify.py" ~/Downloads/asteroidos-rubyfish-2.1/asteroid-rubyfish-boot.img "$B"`
(add `--allow-config-diff` for config-changing branches).

### Kernels with the stock module ABI (`sound` and later)

The `sound` branch turns tracing and module signatures back on as in the
stock Wear OS kernel, so that Mobvoi's audio modules on the vendor partition
(`/vendor/lib/modules/audio_*.ko`) load. That changes `struct module` and
`struct task_struct`, so check 6 reports about 6,000 CRC changes against the
2.1 release, by design, and the release's three rootfs modules no longer load
(nothing loads them). The ABI that matters is now the stock modules'. Check
it with `modcrc.py` before every trial of such a kernel; it exits non-zero on
any mismatch:

```sh
M=~/Documents/sonic-screwdriver/build/kernel/vendor-modules   # git-ignored; never commit these
[ -d "$M" ] || { mkdir -p "$M" && scp -q 'sonic-screwdriver:/vendor/lib/modules/*.ko' "$M/"; }
python3 -I "$T/sonic/modcrc.py" "$B" "$M"
```

It must say 493 match, 0 mismatch, 0 missing (395 symbols resolved between
the modules). The 2.1 release gives 148 match, 343 mismatch, 2 missing.

## Trial boot

Set `B` to the build directory, e.g. `B=~/Documents/sonic-screwdriver/build/kernel/002-asteroidos-2.1`.

1. **Before:** check the watch is on its normal kernel and healthy (the
   `build` line fails on the normal kernel; everything else must be ok):

   ```sh
   "$T/sonic/trial-check.sh" "$B"
   ```

   The reference kernel log is a *complete* normal boot. The journal forgets a
   boot's start after about 10 hours, so use the one saved on 2026-10-30,
   `~/Documents/sonic-screwdriver/build/kernel/logs/klog-normal-boot-2026-10-30-after-trial.txt`,
   or save a new one within a few hours of a normal boot:
   `ssh sonic-screwdriver journalctl -k -b 0 -o short-monotonic --no-pager > ~/Documents/sonic-screwdriver/build/kernel/logs/klog-normal-boot-$(date +%F).txt`.

2. **Write `recovery`** (checks, copy, `dd`, read-back; refuses anything unsafe):

   ```sh
   "$T/sonic/flash-recovery.sh" "$B/boot-trial.img"
   ```

3. **Boot it once:**

   ```sh
   ssh sonic-screwdriver systemctl reboot --reboot-argument=recovery
   until ssh -o ConnectTimeout=8 sonic-screwdriver true 2>/dev/null; do sleep 5; done
   ```

   SSH is back about 20–40 s after the reboot. You now have 10 minutes.

4. **Check it:**

   ```sh
   "$T/sonic/trial-check.sh" "$B"
   ```

   It checks: trial cmdline, `uname -v` = this build, `/proc/config.gz` = the
   build's config, download mode off, dead-man's switch running, Wi-Fi up with
   an address, Bluetooth powered, `ioslink` active (the user's iPhone bridge,
   a user service of `ceres`; the iPhone can take minutes to reconnect, see
   `journalctl -t ioslink`), display (`fb0`, `mdss_fb`, DSI) and touch
   (`bt541_ts`) drivers bound, `asteroid-launcher` running, and no
   oops/BUG/WARN beyond the two the normal kernel also prints at boot.

5. **Compare kernel logs** (normalised; shows messages only one boot has):

   ```sh
   L=~/Documents/sonic-screwdriver/build/kernel/logs
   ssh sonic-screwdriver journalctl -k -b 0 -o short-monotonic --no-pager > "$L/klog-trial-$(basename "$B").txt"
   python3 -I "$T/sonic/klog-compare.py" --window 90 "$L/klog-normal-boot-2026-10-30-after-trial.txt" "$L/klog-trial-$(basename "$B").txt"
   ```

   Wait until the trial has been up for 90 s first, so both logs cover the
   same span. See [Known differences](#known-differences-in-trial-boots) for
   what to expect.

6. **Keep it running** once it is healthy (else it reboots after 10 min):

   ```sh
   "$T/sonic/trial-check.sh" --confirm "$B"
   ```

7. Run the fix's own tests (each `todos/*.md` on its branch says what), and ask
   the user to try the screen and touch by hand.

8. **Back to the normal kernel:** `ssh sonic-screwdriver systemctl reboot`
   (a plain reboot always boots `boot`). `recovery` keeps the trial image; it
   only runs on an explicit reboot into recovery.

To put the release kernel back into `recovery` (it was there before the first
trial; the original contents are in `build/kernel/backups/`):

```sh
R=~/Downloads/asteroidos-rubyfish-2.1/asteroid-rubyfish-boot.img
ssh sonic-screwdriver 'cat > /tmp/r.img' < "$R"
ssh sonic-screwdriver 'P=/dev/block/platform/msm_sdcc.1/by-name/recovery; dd if=/tmp/r.img of=$P bs=1M conv=fsync status=none && sync && head -c 13758464 $P | sha512sum && rm /tmp/r.img'
# must print b834a219865371f8…, the release's SHA-512
```

### Telling the kernel apart from the boot path: the control image

If a trial shows something the normal boot doesn't, find out whether the new
kernel or booting from `recovery` causes it: boot the release kernel itself,
byte for byte, from `recovery` with the trial arguments, and compare.

```sh
C=~/Documents/sonic-screwdriver/build/kernel/control; mkdir -p "$C"
python3 -I "$T/sonic/bootimg.py" unpack ~/Downloads/asteroidos-rubyfish-2.1/asteroid-rubyfish-boot.img "$C/release" > /dev/null
python3 -I "$T/sonic/bootimg.py" pack -o "$C/boot-release-trial.img" --kernel "$C/release/kernel" \
    --ramdisk "$C/release/ramdisk" --cmdline "$(cat "$C/release/cmdline") sonic.trial=1 msm_poweroff.download_mode=0" \
    --board rubyfish --base 0x80000000 --kernel_offset 0x00008000 --ramdisk_offset 0x01000000 \
    --second_offset 0x00f00000 --tags_offset 0x00000100 --pagesize 2048
"$T/sonic/flash-recovery.sh" --control "$C/boot-release-trial.img"
ssh sonic-screwdriver systemctl reboot --reboot-argument=recovery
```

The dead-man's switch applies to it too. Afterwards flash the trial image
again (step 2) to get back to the trial.

## Known differences in trial boots

Established with the baseline on 2026-10-30 (two trial boots of `asteroidos-2.1`,
a fresh normal boot, and a control boot of the release kernel from
`recovery`). None of these is a regression; anything else is.

- **Version strings:** `Linux version …`, `uname -v`, and the `CPU: … Tainted`
  lines that follow the two WARNINGs below.
- **Two WARNINGs at every boot, release kernel included:**
  `sysfs_warn_dup` (`/fs/sysfs/dir.c:30`) and `pm_qos_add_request`
  (`/kernel/power/qos.c:603`). `trial-check.sh` ignores exactly these two.
- **At every screen-off:** `mdss_dsi_panel_set_idle_mode: Idle (0->1)`,
  `mdss_resync_timeline: …`, `mdss_mdp_display_commit: ctl 0 error displaying
  frame`, then `nanohub: … [LCD-D] lcd pwr on` (the second display takes
  over). The release kernel prints the same after a fresh boot.
- **Sometimes at boot:** right after `mdss_mdp_overlay_kickoff: unable to start
  overlay 0 (-1)` (printed by every boot), `wait4pingpong timed out` →
  `mdss_fb_report_panel_dead: Panel has gone bad … PANEL_ALIVE=0` →
  `mdp-fence: frame timeout`, `mdss_dsi_cmd_mdp_busy: timeout error`. Seen in 1
  of 2 trial boots of the baseline, whose kernel is code-identical to the
  release, and 0 of 2 release boots: a race at the first frame, not the build.
  `mdss_fb` clears the dead-panel state at the next unblank, but until then it
  holds back brightness changes, so **before testing brightness after such a
  boot, blank and unblank once** (`mcetool --blank-screen; mcetool --unblank-screen`).
- **Environment noise:** Wi-Fi association and deauthentication messages (the
  access point's doing), USB gadget lines while the cable is plugged in, touch
  `mini_init_touch` / `zinitix_resume_proc err` (the known touch-driver bug),
  nanohub battery and sensor lines, `IRQn no longer affine to CPUn`,
  `spmi spmi-0: cleanup_irq …` (some boots, the release kernel's too), and
  systemd messages that reach the kernel log at different moments.
- **Cable unplugged:** the reference boot above was on the USB cable. Off the
  cable the watch already suspends in the first 90 s, so a trial boot then also
  shows suspend and resume lines (`PM: suspend entry`/`exit`, `Freezing …`,
  `dhdsdio_suspend`/`resume Enter`, `gic_show_resume_irq`, wakeup sources)
  and lacks the USB ones (`android_work`, `msm_hsusb`, `configfs-gadget`,
  `ipa_get_transport_type … IPA HW is not supported`). Seen with
  `brightness-fix` on 2026-10-30.
- **Kernels with the stock module ABI** (`sound` and later) add, at every
  boot: the `trace_printk() being used. Allocating extra memory.` NOTICE
  banner (Qualcomm's `msm_bus_fabric_adhoc.c` uses `trace_printk()`, as in
  stock; the trace buffer then takes 1.4 MB per CPU), `Loaded X.509 cert
  'Build time autogenerated kernel key: …'` (the build's throwaway module
  signing key), and systemd mounting the kernel trace file system instead of
  skipping it. Seen with `sound` on 2026-10-30.
- **Kernels with the speaker cap** (`sound`, from build 020) add, at every
  boot: `i2c-speaker-cap: speaker amplifier 2-0034 held at least 20.0 dB below
  full gain` (22.0 in builds 020 and 021). Seen with `sound` on 2026-10-30.

## Gotchas

- `systemctl reboot recovery` fails on the watch's systemd 257 ("Too many
  arguments."); use `systemctl reboot --reboot-argument=recovery`.
- **A boot that starts on the USB cable never suspends, not even after
  unplugging.** mce starts a CPU keepalive when it sees the charger at boot
  and doesn't stop it (`journalctl -u mce`: "keepalive started", no "stopped
  after"), so its `mce_mux` wakelock stays held. Off the cable at boot, the
  same keepalive stops after 13 s. A test that needs real system suspends must
  boot off the cable: unplug first, then reboot over Wi-Fi.
- **connman doesn't retry a failed Wi-Fi connection at boot.** If the first
  association fails (out of range, or the access point rejects the WPA
  handshake; `journalctl -u connman`: `error "connect-failed"`, then `state is
  failure`), wlan0 stays down and SSH is gone for the whole boot. Over adb,
  `connmanctl scan wifi` sometimes makes connman reconnect; if `connmanctl
  services` still shows the saved network without `R`/`O`, connect it:
  `connmanctl connect $(connmanctl services | awk '/^\*A/ {print $NF; exit}')`.
  Same address, nothing in the settings changes. Keep the watch in range while
  it boots. Seen three times on 2026-10-30. On trial boots,
  `kernel-trial-wifi.service` does both by itself for 2 minutes.
- **An unconfirmed trial whose Wi-Fi is down** can still be confirmed over adb
  before the dead-man's switch fires: check it by hand (`uname -v`, `dmesg` for
  oops, the touch driver bound, the launcher active), then
  `adb shell touch /run/sonic-trial-ok`.
- The journal is persistent but keeps only about 10 hours: the kernel logs
  every suspend/resume (`gic_show_resume_irq`, …) and journald vacuums old
  entries at ~480 MB. The boot-time messages of a long-running boot are gone;
  save logs soon after booting.
- `kbuild.sh` resets `/work/src` to the branch (`checkout --force`,
  `clean -fdx`). It refuses to run over uncommitted changes; commit and push
  them first, or build them with `--worktree`.
- The build leaves an untracked file `drivers/platform/msm/ipa/ipa_common`
  (a quirk of Qualcomm's Makefile). Commit with `git commit -a` or name your
  files; never `git add -A`.
- `journalctl -k -o short-monotonic` timestamps of messages printed before
  journald starts (~5.7 s) are the time journald read them, not when they were
  printed.
- The kernel's own `zImage-dtb` appends DTBs in `find` order; we assemble it
  in the release's order ourselves (the build log says whether they agreed).
- Debian's `mkbootimg` (installed in the image for `unpack_bootimg`) is not
  used for packing: it writes `second_addr` 0. `bootimg.py pack` matches the
  release byte for byte.
- The cross compiler's prefix is `arm-linux-gnueabi-`, not `…gnueabihf-`: a
  kernel-only toolchain. The float ABI default doesn't matter, the kernel
  is built with `-msoft-float`.
- The build ID (`.notes`, 20 bytes, `/sys/kernel/notes`) always differs from
  the release's: it hashes all of `vmlinux`, debug info with build paths
  included. Everything else in a `--repro` build of `asteroidos-2.1` is
  identical to the release kernel.
- GCC 14 prints many warnings for this 4.9 tree (`-Warray-compare`,
  executable-stack notes from `ld`, deprecated OpenSSL in `extract-cert`);
  AsteroidOS's build has the same. Errors stop the build.
- `adb` over the USB cable gives a root shell (`adb shell`) on both kernels;
  there is no USB networking (`usb-moded` fails on rover).

## Files and variables

| File | Runs on | Purpose |
|---|---|---|
| `Dockerfile` | Docker | build environment (debian:trixie + GCC 14.3.0/binutils 2.44 + tools) |
| `kbuild.sh` | Mac | build a branch in Docker, then `verify.py` |
| `build-kernel.sh` | container | the build itself (called by `kbuild.sh`) |
| `kshell.sh` | Mac | shell or command in the container's source tree |
| `verify.py` | either | equivalence checks against the release |
| `modcrc.py` | either | would given modules (the stock audio ones) pass MODVERSIONS on a build? |
| `bootimg.py` | either | inspect, split, pack and compare boot images |
| `flash-recovery.sh` | Mac | write a trial image to `recovery` over SSH, with safety checks |
| `trial-check.sh` | Mac | health check of the running kernel over SSH; `--confirm` |
| `klog-compare.py` | Mac | diff two kernel logs |
| `kernel-trial.service` | watch | the dead-man's switch |
| `kernel-trial-wifi.service` | watch | reconnects the saved Wi-Fi network if a trial boots without it |

| Variable | Default |
|---|---|
| `SONIC_REPO` | `~/Documents/mobvoi-ticwatch-kernel.git` (bare repo) |
| `SONIC_OUT` | `~/Documents/sonic-screwdriver/build/kernel` (outputs; git-ignored in the project repo) |
| `SONIC_RELEASE_IMG` | `~/Downloads/asteroidos-rubyfish-2.1/asteroid-rubyfish-boot.img` |
| `SONIC_HOST` | `sonic-screwdriver` (SSH alias, root, key-only) |
| `SONIC_MAC_DIR` | unset; `kshell.sh` mounts it at `/mac` |

Reference values: release boot image SHA-512 `b834a219…bb593bf3` (13758464
bytes: zImage 8031920 + rover.dtb 285107 + rubyfish.dtb 285107, ramdisk
5151889); `recovery` is `mmcblk0p27`, `boot` is `mmcblk0p26`, both 32 MiB.

## Baseline result (2026-10-30)

`asteroidos-2.1` @ `4fffe77`, before this directory was added:

- **Build 001, `--repro`:** the decompressed kernel Image (19542020 bytes) is
  identical to the release kernel's except the 20-byte build ID; same config
  (header included), same DTBs in the same order, same ramdisk and header
  values, 9046/9046 export CRCs equal. The compressed zImage is identical up
  to the build ID.
- **Build 002, `#2-4fffe77 SMP PREEMPT Fri Oct 30 10:05:14 UTC 2026`:** all
  checks pass. It differs from build 001 only by its version string: ccache
  recompiled 3 objects, `System.map` lists the same symbols, of which 57 in
  `.rodata` after `linux_banner` move by a few bytes.
- **Trial boot 1:** healthy (Wi-Fi, Bluetooth, ioslink with the iPhone
  reconnected, display, touch, launcher), but with the boot-time dead-panel
  messages; left unconfirmed while they were investigated, so the dead-man's
  switch rebooted it after 600 s into the normal kernel, as designed.
- **Control boot** of the release kernel from `recovery` and a fresh normal
  boot, for comparison (see Known differences).
- **Trial boot 2:** healthy, no kernel messages beyond the known differences
  in the first 90 s; confirmed and left running for the user to try the screen
  and touch by hand.
