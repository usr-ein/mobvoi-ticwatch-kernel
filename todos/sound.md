# Sound fix (to do)

Make the speaker and the two microphones work. Audio has never worked on this port; AsteroidOS rates both "Bad" for rubyfish.

Status (2026-10-29): diagnosed, nothing changed on the watch. **It needs a kernel change**: a config change plus a reworked AsteroidOS patch. Build it in the same kernel as [brightness_fix.md](brightness_fix.md) and [touch_driver_fix.md](touch_driver_fix.md) (branches `brightness-fix` and `touch-driver-fix`), then wire up userspace.

Paths under `stock-backup/` are in `~/Documents/sonic-screwdriver/`.

## Verdict

- **The hardware is fine.** The device tree describes it correctly, and the firmware and calibration files are already on the watch.
- **All of Qualcomm's audio drivers live outside the kernel.** They are 25 modules on the vendor partition (`/vendor/lib/modules/audio_*.ko`). Mobvoi built them, and they include custom code for this watch's amplifier.
  - Stock Wear OS loads these modules at boot. AsteroidOS never does.
- **They can't load into the AsteroidOS kernel as built.**
  - AsteroidOS turned off kernel tracing (`0001-Disable-tracing.patch`), which changed the kernel's interface to modules.
  - The modules need 2 kernel symbols that no longer exist.
  - 343 of the remaining 491 symbol checksums no longer match what the modules were built against.
- **Fix:**
  1. Rebuild the kernel with stock's tracing settings so the stock modules fit.
  2. Load the modules and start the DSP from userspace.
  3. Give PulseAudio a real output.
- **No device-tree change is needed, and no driver source.**

## Hardware path

| Part | What it is | Evidence |
|---|---|---|
| Codec | The SDM429W's internal **digital** codec only. There is no analog codec, so no headset. | Device tree: `msm_dig_codec` @ `c0f0000`, `qcom,no-analog-codec` |
| Microphones | **Two digital mics**, DMIC1 and DMIC2. Each feeds the codec's decimator (DEC1/DEC2), which sends audio to the DSP over the tertiary I2S link (`TERT_MI2S_TX`). Stock uses DMIC1 for recording. | Sound node routing `DMIC1 → Digital Mic1`, `DMIC2 → Digital Mic2`. Stock `mixer_paths_sdm429w.xml`: `DEC1 MUX = DMIC1`, `MultiMedia1 Mixer TERT_MI2S_TX = 1` |
| Speaker amp | **Awinic AW882xx** smart amplifier on I2C bus 2, address `0x34`, fed by the quaternary I2S link (`QUAT_MI2S`). The 2023 port attempt calls it an AW88230. | DT `aw882xx_smartpa@34` (`awinic,aw882xx_smartpa`, status okay, reset GPIO 92, IRQ GPIO 93) under `i2c@78b6000`. Sound node: `qcom,msm-ext-pa = "quaternary"`, flag `ext_pa_aw882xx`. Watch: `/sys/bus/i2c/devices/2-0034` exists, with no driver |
| NXP TFA98xx | An alternative amplifier footprint, **disabled**. The kernel still builds a `tfa98xx` driver (log: `TFA98XX driver version v6.6.3`), which does no harm. | DT `nxp_smartpa@34`, status disabled |
| ADSP | The audio DSP (LPASS/QDSP6) does all mixing and routing. **No sound is possible until it boots.** | DT: `qcom,lpass@c200000` (firmware `adsp`); `qcom,msm-adsp-loader` with `qcom,adsp-state = <0>`, meaning userspace must start it; `qcom,msm-audio-apr` |
| "BG" co-processor | **Not involved.** "BG" in the model string comes from Qualcomm's reference design. All BG nodes are disabled and their drivers aren't built. | DT: `qcom,bg-daemon`, `qcom,bg-rsb`, `qcom,glink-bgcom-xprt-bg` all `status = "disabled"`. Config: `MSM_BGCOM`, `MSM_BGRSB`, `MSM_PIL_SSR_BG` off |

The sound card node is `c051000.sound`, with `compatible = "qcom,msm8952-dig-asoc-snd"` and `qcom,model = "sdm429w-snd-card"`.

## Current state (evidence)

### Kernel

- **No sound card.**
  - `/proc/asound/cards` says `--- no soundcards ---`, and `/dev/snd` holds only `timer`.
  - Boot log: `[5.701134] No soundcards found.`
  - There are no messages at all from the DSP link, the DSP loader or the sound card driver (`journalctl -k -b 0`).
- **The kernel source has no Qualcomm audio drivers.** `mobvoi-ticwatch-kernel` @ `c428ef36` has no `sound/soc/msm`, no `techpack/audio` and no AW882xx driver. Of the audio drivers, the kernel builds only `SND_SOC_TFA98XX` and a register-map helper for the digital codec.
- **The audio devices exist but have no drivers.** The device tree creates `c051000.sound`, `c0f0000.msm_dig_codec`, `soc:qcom,msm-adsp-loader`, `soc:qcom,msm-audio-apr` and all the `msm-pcm-*`/`msm-dai-*` devices. None has a driver. The amplifier shows up as `2-0034 name=aw882xx_smartpa driver=(none)`.
- **The ADSP has never started.**
  - `/sys/bus/msm_subsys/devices/subsys1` (`adsp`) is `OFFLINING`, the same as the modem and Wi-Fi DSPs, which this watch never uses.
  - `/sys/kernel/boot_adsp` doesn't exist.
- **No modules are loaded** (`/proc/modules` is empty).
- **The kernel is 32-bit ARM**, not arm64: `uname -m` says `armv7l`, the config has `CONFIG_ARM=y`, and the stock modules are `ELF 32-bit … ARM`.

### How stock Wear OS did it (from `stock-backup/vendor.img`)

1. **Load the modules.** `/vendor/etc/init/hw/init.target.rc`, `on early-init`, runs `modprobe -a -d /vendor/lib/modules` on all 25 `audio_*` modules.
2. **Start the ADSP.** `init.qcom.rc`, `on early-boot`, runs `write /sys/kernel/boot_adsp/boot 1`.
   - The ADSP firmware comes from `firmware_class.path=/vendor/firmware_mnt/image`, set on the stock kernel command line.
3. **Run the audio HAL.** The `vendor.audio-hal-2-0` service runs `/vendor/lib/hw/audio.primary.rover.so`.
   - This is Qualcomm's standard `hardware/qcom/sdm429w/audio` HAL.
   - It's configured by `mixer_paths_sdm429w.xml` and `audio_platform_info.xml`.
   - It loads calibration from `MTP_SDM429W_*.acdb` through `libacdbloader.so`.

Which module drives which device:

| Device | Module | Notes |
|---|---|---|
| `qcom,msm8952-dig-asoc-snd` (the sound card) | `audio_machine` | Has Mobvoi-only code that reads `ext_pa_aw882xx` and links the amplifier (`aw882xx-aif`) into the card |
| `i2c:aw882xx_smartpa` (the amplifier) | `audio_AW882xx` | Loads `/vendor/firmware/aw882xx_spk_reg.bin` and its calibration `/mnt/vendor/persist/aw_cali.bin` |
| `qcom,adsp-loader` | `audio_adsp_loader` | Creates `/sys/kernel/boot_adsp/boot` |

### Why the stock modules can't load into the AsteroidOS kernel

- **Version string: not a blocker.**
  - Each module records the kernel version it was built for: `4.9.232-perf+ SMP preempt mod_unload modversions ARMv7 p2v8`. The AsteroidOS kernel reports `4.9.232 SMP preempt mod_unload modversions ARMv7 p2v8`.
  - `same_magic()` in `kernel/module.c` ignores the release part (`4.9.232-perf+` vs `4.9.232`) whenever symbol checksums (`MODVERSIONS`) are in use. The per-symbol checksums decide instead.
- **Two symbols are missing.**
  - The modules use 492 kernel symbols. The running kernel exports all but two: `kmem_cache_alloc_trace` and `kmalloc_order_trace`.
  - Both are exported only under `#ifdef CONFIG_TRACING` (`mm/slub.c:2785`, `mm/slab_common.c:1051`).
  - So loading fails with "Unknown symbol", and forcing doesn't help.
- **343 symbol checksums don't match.**
  - With `MODVERSIONS`, each exported symbol carries a checksum (CRC) of its type definitions, and every module records the CRCs it was built against. I read both tables out of the boot images and the modules.
  - 493 checks in total: the 492 symbols plus `module_layout`, which covers `struct module` itself.

  | Kernel | Match | Mismatch | Missing |
  |---|---|---|---|
  | Stock | 493 | 0 | 0 |
  | AsteroidOS 2.1 | 148 | 343 (including `module_layout`) | 2 |

  - The stock row checks the method itself.
  - The two kernels export the same symbols except 185 that only stock has, and all 185 depend on the config: tracing, USB NCM, module signing. So it's the same source with a different config.
  - The mismatches come from two structures that change shape with this config:
    - `struct module` has fields under `CONFIG_MODULE_SIG`, `TRACEPOINTS`, `TRACING` and `EVENT_TRACING`.
    - `struct task_struct` has fields under `CONFIG_TRACE_IRQFLAGS` and `TRACING`.
  - Almost every kernel API refers to one of them.
- **Why the configs differ.** AsteroidOS's `0001-Disable-tracing.patch` and its defconfig (`# CONFIG_FTRACE is not set`) turned tracing off, because the GPU (KGSL/Adreno) and IPA trace code "seem to cause compilation issues".
  - The stock and AsteroidOS configs differ in 80 options, **none of them audio**.
  - Only three groups matter for modules: the tracing options, module signing, and `LOCALVERSION`.
  - The rest are Bluetooth, devtmpfs, tmpfs ACLs, USB NCM, the command line and `PANIC_ON_OOPS`.
- **Compilers:** stock was built with gcc 4.9, AsteroidOS with gcc 14.3.

### Firmware: nothing missing

- **ADSP firmware.**
  - The modem partition (`mmcblk0p1`, FAT) is mounted at `/vendor/firmware_mnt` and contains `image/adsp.mdt` and `adsp.b00`–`b09`.
  - AsteroidOS's `firmwared` is running and serves `/vendor/firmware:/vendor/firmware_mnt/image:…` to the kernel, which asks userspace for firmware when it can't find a file itself (`FW_LOADER_USER_HELPER_FALLBACK=y`).
- **Amplifier files.** `/vendor/firmware/aw882xx_spk_reg.bin` and `aw882xx_rcv_reg.bin` are present.
  - The factory calibration `/persist/aw_cali.bin` is present (5 bytes, dated 2021).
  - rubyfish's `init.rc` already links `/mnt/vendor/persist` to `/persist`, so the path the module expects works.
- **Unrelated boot error.** The `venus: Failed to locate venus.mdt(rc:-11)` lines at boot are about the video decoder. Rover simply has no video firmware.

### Userspace

- **Nothing starts the audio stack.** AsteroidOS runs Android's init with its own `/init.rc`, plus only the service files in `/usr/libexec/hal-droid/system/etc/init/` (Bluetooth, graphics, power, …).
  - It never reads the vendor's `init.target.rc` or `init.qcom.rc`.
  - So nothing loads the modules, starts the ADSP, or runs an audio HAL.
- **PulseAudio has no real output.** PulseAudio 17.0 runs as ceres's user service, and its only sink is `auto_null` ("Dummy Output").
  - `/etc/pulse/default.pa` loads `module-droid-card` only if it exists. meta-rubyfish removes the package that provides it, `pulseaudio-modules-droid-jb2q`, in [commit 4933fa7](https://github.com/AsteroidOS/meta-smartwatch/commit/4933fa7dc0): "Audio doesn't yet work, this avoids pulseaudio from crashing repeatedly".
  - `module-udev-detect` and `module-alsa-*` are installed but never loaded.
  - The journal error `Failed to open module module-keepalive.so` happens because that module comes with the droid package.
- **AsteroidOS doesn't know there's a speaker.** `/etc/asteroid/machine.conf` has no `HAS_SPEAKER`, because PR #283 removed `MACHINE_HAS_SPEAKER` from `rubyfish.conf`.
- **Notification sounds need no change.** rubyfish's `ngfd.ini` already plays them through GStreamer (`gst`), so they'll work once a real output exists.
- **Tools:** the watch has `pactl` and GStreamer (`pulsesrc`, `pulsesink`), but no `paplay`, `aplay`, `amixer` or `tinymix`. ceres is in the `audio` group.
- **Tests run** (one of each, as allowed):
  - Playing `/usr/share/sounds/notification.wav` with `gst-launch-1.0 … ! pulsesink` succeeded, into `auto_null`, with no sound.
  - A 3 s `pulsesrc` recording read from `auto_null.monitor` and came back as pure digital silence (peak 0). The file was deleted from the watch.

### Earlier attempts

- **[PR #227](https://github.com/AsteroidOS/meta-smartwatch/pull/227)** (argosphil, 2023; replaced by #283) built the audio drivers from source.
  - Sources: Google's [`kernel/msm-extra` `android-msm-beluga-4.9-pie-wear-mr2`](https://android.googlesource.com/kernel/msm-extra/+/refs/heads/android-msm-beluga-4.9-pie-wear-mr2) (the OPPO Watch's audio drivers), plus Awinic's public `aw88230` driver.
  - It was dropped as not working ("Removed work that doesn't work or hasn't been tested (like audio, nfc)").
  - That source's sound card driver (`msm8952.c`) lacks Mobvoi's amplifier code, so it couldn't drive this amp unchanged.
- **The community "audio fix"** ([issue #175 comment](https://github.com/AsteroidOS/meta-smartwatch/issues/175#issuecomment-5243272038), yesimxev, 2026-08-10) is [commit b168859a](https://github.com/yesimxev/mobvoi-ticwatch-kernel/commit/b168859a372f0d411ce7a4da78c5337ef0a0bab0), "Bypass signed vendor modules load".
  - **What it's for:** a Kali NetHunter kernel on **Wear OS**. It lets that self-built kernel load the stock vendor modules.
  - **What it changes:**
    - `nethunter_defconfig`: module signing off, `MODVERSIONS` off, `MODULE_FORCE_LOAD` on.
    - `kernel/module.c`: a version-string mismatch is only logged.
  - **What it keeps:** stock's tracing settings, unchanged. That's why the modules work there.
  - **For AsteroidOS it only partly applies:**
    - AsteroidOS already has signing off and force-loading on.
    - The version string doesn't block loading here (see above).
    - The real blocker, tracing, isn't something the patch touches.
  - Its approach, stock modules on a kernel that matches them, is what this plan builds on.
  - The zip itself is a Wear OS kernel. It lacks AsteroidOS's patches, including `0003-initramfs-Don-t-skip-initramfs`, which is needed because the bootloader passes `skip_initramfs`. So it isn't usable here as is.

## Root causes

1. **The drivers aren't in the kernel.** They exist only as Mobvoi's binary vendor modules. Mobvoi's code for the AW882xx amplifier and the sound card isn't in any public source.
2. **The AsteroidOS kernel doesn't match those modules,** because tracing is disabled: 2 symbols missing, 343 checksum mismatches.
3. **Nothing loads the modules or starts the ADSP.** meta-rubyfish's `init.rc` does neither, unlike meta-beluga's.
4. **PulseAudio has no real output.** The droid module was removed, ALSA detection is off, and `MACHINE_HAS_SPEAKER` isn't set.

## Plan

### Kernel (one build with the brightness and touch fixes)

Build, test-boot and flash as in [brightness_fix.md](brightness_fix.md#plan-build-test-flash). Two corrections to that plan apply to all three fixes:

- **The kernel is 32-bit ARM, not arm64.**
  - Cross-compile with `ARCH=arm` and an `arm-linux-gnueabihf` toolchain.
  - Run `make ARCH=arm olddefconfig`, not `ARCH=arm64`.
  - The output is a zImage with the device trees appended.
- **`fastboot boot` may not work on this bootloader.** argosphil's 2023 notes on rubyfish say "fastboot boot doesn't work". It's untested on rover's `rover-03.02.38.03.16`.
  - **Fallback:** flash the test image to **recovery only**, then choose Recovery mode in the bootloader menu.
  - **Rollback:** `boot` keeps the 2.1 kernel, so a normal reboot goes back to it.

Steps:

1. **Restore stock's tracing settings.** Effort: medium (build iterations). Risk: low at runtime, since stock runs these settings; the gcc 14 build may need fixes.
   - In the defconfig (`arch/arm/configs/asteroidos-rubyfish_defconfig` here, meta-rubyfish's `defconfig` upstream), replace `# CONFIG_FTRACE is not set` with stock's tracer settings:
     ```
     CONFIG_FTRACE=y
     # CONFIG_FUNCTION_TRACER is not set
     # CONFIG_IRQSOFF_TRACER is not set
     # CONFIG_PREEMPT_TRACER is not set
     # CONFIG_SCHED_TRACER is not set
     # CONFIG_HWLAT_TRACER is not set
     CONFIG_FTRACE_SYSCALLS=y
     # CONFIG_TRACER_SNAPSHOT is not set
     CONFIG_BRANCH_PROFILE_NONE=y
     # CONFIG_STACK_TRACER is not set
     # CONFIG_BLK_DEV_IO_TRACE is not set
     # CONFIG_UPROBE_EVENT is not set
     # CONFIG_PROBE_EVENTS is not set
     CONFIG_PREEMPTIRQ_EVENTS=y
     # CONFIG_TRACEPOINT_BENCHMARK is not set
     # CONFIG_RING_BUFFER_BENCHMARK is not set
     ```
   - After `olddefconfig`, these must be `=y`, exactly as in stock: `TRACING`, `TRACEPOINTS`, `EVENT_TRACING`, `TRACE_IRQFLAGS`, `CONTEXT_SWITCH_TRACER`, `GENERIC_TRACER`, `NOP_TRACER`, `RING_BUFFER`, `TRACE_CLOCK`, `BINARY_PRINTF`, `TRACING_EVENTS_GPIO`.
     - Diff the result against the stock config. Extract it from `stock-backup/boot.img` with `scripts/extract-ikconfig`, or use the copy in my scratch folder (below).
   - **Rework `0001-Disable-tracing.patch`** (the `Disable tracing` commit on `asteroidos-2.1`) so the GPU and IPA code stays free of tracepoints without turning tracing off for the whole kernel:
     - keep its existing edits;
     - add `ccflags-y += -DNOTRACE` to `drivers/gpu/msm/Makefile` and `drivers/platform/msm/ipa/ipa_v2/Makefile`.
     - With `NOTRACE` defined, `include/linux/tracepoint.h:113-116` turns every `trace_*()` call in those files into an empty stub.
     - If the build breaks in other trace headers, treat them the same way.
2. **Make `struct module` identical to stock.** Effort: small. Risk: low.
   - **Preferred:** `CONFIG_MODULE_SIG=y` and `CONFIG_MODULE_SIG_SHA512=y` (as in stock), with `MODULE_SIG_FORCE` and `MODULE_SIG_ALL` off.
     - The stock modules are signed with Mobvoi's key ("Module signature appended"). Without enforcement, a key the kernel doesn't know only marks the kernel as tainted ("module verification failed: signature and/or required key missing"), and the module still loads.
     - The SHA-512, RSA, X.509 and PKCS#7 support this needs is already enabled.
     - The checksum check stays on and proves compatibility symbol by symbol.
     - The build needs `openssl` to generate a throwaway signing key.
   - **Fallback (yesimxev's approach):** `# CONFIG_MODVERSIONS is not set`, plus his one-line change to `kernel/module.c`. This is quicker but removes the safety check.
   - **Keep `LOCALVERSION` empty,** so `uname -r` stays `4.9.232` and `/lib/modules/4.9.232` keeps working.
3. **Check compatibility before booting.** This runs offline; the watch isn't needed.
   - Run my script on the new boot image. It compares the kernel's symbol checksums with what the stock modules expect:
     ```
     python3 -I ~/.cache/sonic-screwdriver/research-sound/scripts/crc_compare.py \
         stock-backup/boot.img new-boot.img <stock modules dir> <out dir>
     ```
     The stock modules can be extracted with `debugfs -R 'rdump /lib/modules <dir>' stock-backup/vendor.img`.
   - **Target:** for the new image, 493 match and 0 mismatch. The "not exported by kernel" count should equal stock's (395); those are symbols the modules provide to each other. Today's 2.1 image gives 148 match, 343 mismatch, and 397 not exported (the 2 missing symbols).
   - Any mismatch names a structure that still differs. Fix that first, or knowingly switch to step 2's fallback.
   - Keep this compatibility in mind for the brightness and touch patches too. They don't change any structure modules use.
4. **No device-tree change.** `0005-dts-Add-stock-rover-device-tree.patch` already describes the audio hardware exactly as stock does.
5. **Optional:** add `firmware_class.path=/vendor/firmware_mnt/image` to the command line in `img_info`, so the kernel loads the ADSP firmware directly instead of through `firmwared`. Not required.

### Firmware

Nothing to do. See [Firmware: nothing missing](#firmware-nothing-missing).

### Userspace (after the patched kernel boots)

1. **Load the modules and start the ADSP.** Effort: small. Risk: low.
   - If anything goes wrong, the kernel's `PANIC_ON_OOPS=y` reboots the watch. In a test boot, that falls back to the installed kernel.
   - **First, by hand,** as root, in the test boot:
     ```sh
     cd /vendor/lib/modules
     for m in audio_q6_notifier audio_apr audio_q6 audio_AW882xx audio_adsp_loader audio_usf \
              audio_platform audio_native audio_pinctrl_wcd audio_swr audio_swr_ctrl audio_hdmi \
              audio_wcd_core audio_wcd9xxx audio_wsa881x audio_wsa881x_analog audio_mbhc audio_stub \
              audio_digital_cdc audio_analog_cdc audio_wcd_cpe audio_cpe_lsm audio_wcd9335 \
              audio_machine audio_machine_ext; do
       insmod $m.ko || { echo "FAILED at $m"; break; }
     done
     echo 1 > /sys/kernel/boot_adsp/boot
     ```
     - This is stock's full list, in dependency order from `modules.dep`.
     - Seven of them serve the disabled tasha card and can be dropped later: `audio_usf`, `audio_swr_ctrl`, `audio_hdmi`, `audio_wcd_cpe`, `audio_cpe_lsm`, `audio_wcd9335` and `audio_machine_ext`.
   - **Expect, within a few seconds:**
     - `cat /sys/bus/msm_subsys/devices/subsys1/state` shows `ONLINE`;
     - `/proc/asound/cards` lists `sdm429w-snd-card`;
     - `/dev/snd/pcmC0D*` and `controlC0` exist;
     - `2-0034` is bound to the amplifier driver.
   - **Make it permanent on this watch** with a systemd oneshot unit that runs the same loop:
     - `RequiresMountsFor=/vendor/lib/modules /vendor/firmware_mnt/image`, `After=android-init.service firmwared.service`, `Before=user@1000.service`;
     - guarded by `grep -q ' kmem_cache_alloc_trace$' /proc/kallsyms`, so it does nothing on an unpatched kernel, for example after a rollback;
     - stopping at the first failed `insmod` and logging it, without failing the boot.
   - **Upstream:** add `insmod` lines and `write /sys/kernel/boot_adsp/boot 1` to `on init` in meta-rubyfish's `init.rc`, as meta-beluga does.
2. **Check sound with plain ALSA,** without the HAL. Effort: small. Risk: low.
   - Install `alsa-utils-amixer` and `alsa-utils-aplay`. They're version 1.2.13, matching the installed libasound, and are in the 2.0 feed the watch already uses.
   - Then, using the stock mixer paths, test the speaker at low volume first:
     ```sh
     amixer -c0 cset name='QUAT_MI2S_RX Audio Mixer MultiMedia1' 1
     aplay -D hw:0,0 /usr/share/sounds/notification.wav        # speaker
     amixer -c0 cset name='DEC1 MUX' DMIC1
     amixer -c0 cset name='MultiMedia1 Mixer TERT_MI2S_TX' 1
     arecord -D hw:0,0 -f S16_LE -r 48000 -c 1 -d 3 /tmp/mic.wav # DMIC1
     ```
   - Device 0 is normally MultiMedia1; check `/proc/asound/pcm`.
   - **If the amp stays silent:** look in dmesg for the AW882xx chip ID and register loading (`aw882xx_load_reg`), and for its switch controls (`amixer -c0 contents | grep -i aw`).
3. **Give PulseAudio a real device.** Pick one of two routes.
   - **a) ALSA (simplest).** Effort: small. Risk: low.
     - The boot script from step 1 also sets the two mixer routes.
     - `~ceres/.config/pulse/default.pa` contains `.include /etc/pulse/default.pa`, then `.nofail`, `load-module module-alsa-sink device=hw:0,0` and `load-module module-alsa-source device=hw:0,0`, and sets both as defaults.
     - No calibration data is used: fine for notifications, possibly weaker mic quality.
   - **b) Android HAL through the droid module** (what AsteroidOS does on other hybris watches, such as [meta-beluga](https://github.com/AsteroidOS/meta-smartwatch/tree/master/meta-beluga)). Effort: medium. Risk: medium.
     - Bring back `pulseaudio-modules-droid-jb2q` and `pulseaudio-module-keepalive` by deleting meta-rubyfish's `pulseaudio_%.bbappend`.
     - beluga's `quirks=+unload_call_exit` may also be needed.
     - The HAL is found through `ro.product.board=rover`. It uses `mixer_paths_sdm429w.xml` and loads the calibration data, so routing, EQ and mic processing match stock.
     - The HAL could still crash PulseAudio; that's why it was removed.
     - The `-jb2q` package isn't in any feed this watch can reach (the 2.1 feed returns 404; 2.0 has only the older `pulseaudio-modules-droid`), so this route means an AsteroidOS build.
4. **Tell AsteroidOS there's a speaker.** Effort: tiny.
   - In meta-rubyfish: `MACHINE_HAS_SPEAKER = "true"` in `rubyfish.conf`.
   - On this watch: add `HAS_SPEAKER = true` under `[Capabilities]` in `/etc/asteroid/machine.conf`.
5. **Send it upstream.** No vendor files need redistributing, because the modules come from the watch's own vendor partition.
   - A meta-smartwatch PR: defconfig, reworked 0001, `init.rc` lines, the PulseAudio change, `MACHINE_HAS_SPEAKER`.
   - Reference it in [meta-smartwatch#175](https://github.com/AsteroidOS/meta-smartwatch/issues/175) and [asteroid#355](https://github.com/AsteroidOS/asteroid/issues/355).

### Alternatives considered

- **Building the drivers from source** (PR #227's route). Effort: large.
  - Use msm-extra beluga-mr2 with its `sdm450` config.
  - Port Mobvoi's amplifier glue (`ext_pa_aw882xx`, linking `aw882xx-aif` to quaternary I2S) into `msm8952.c`.
  - Adapt Awinic's `aw88230` driver, including its connection to the ADSP.
  - This is fully open source, but it has many unknowns, and it failed before. Only worth it if the stock modules can't work.
- **Keeping tracing off, adding the two missing exports, and forcing the load** (`modprobe -f`). Not recommended.
  - The stock modules would run against a different `task_struct` and `struct module` layout, which risks silent memory corruption.
  - It's acceptable only as a throwaway experiment in a test boot.

## How to test

1. Build the shared kernel (brightness + touch + this) and run the compatibility check (kernel step 3).
2. Boot it without flashing: `fastboot boot`, or the recovery fallback above.
   - Check that `uname -v` shows the new build.
   - Check that `grep -c ' kmem_cache_alloc_trace$' /proc/kallsyms` prints `1`.
3. Do userspace step 1 by hand.
   - dmesg must show no "Unknown symbol" and no "disagrees about version".
   - `subsys1` must be `ONLINE`, and `/proc/asound/cards` must list `sdm429w-snd-card`.
4. Do userspace step 2. The speaker must play, and the mic recording must not be silent: speak into it and check the peak is above 0.
5. **Sleep and wake:** let the watch suspend for a few minutes, then play again.
   - Watch dmesg for ADSP restarts (`subsys-restart`).
   - Compare battery drain overnight with today, since the ADSP will now be running.
6. Then:
   1. flash the kernel (together with the other fixes);
   2. make the userspace changes permanent;
   3. set up PulseAudio (step 3);
   4. check that a phone notification makes a sound through ngfd.

## Open questions

- **Does the gcc 14 build work with tracing back on?** 0001 suggests the GPU, Adreno and IPA trace code broke. Other trace headers might too.
- **Do the checksums match exactly** after kernel steps 1–2? They should (same source, same config), but the first build will tell.
- **Does `fastboot boot` work** on `rover-03.02.38.03.16`?
- **Does the amp need the HAL and calibration data?** `audio_AW882xx` talks to an Awinic algorithm on the ADSP (`awinic_algo`, through `audio_q6`).
  - With plain ALSA, the amp might run in a basic mode, or stay muted until its `aw882xx_*_switch` controls are set.
  - Userspace step 2 will show which.
- **Which AW882xx is it?** The device tree says AW882xx; argosphil's recipe says AW88230. The chip-ID line in dmesg will tell.
- **TFA leftovers.** The stock mixer paths set `mono Profile` and `mono Music Playback Volume`, which are TFA98xx controls that won't exist with this amp. The HAL should just log and skip them; check this on the HAL route.
- **Power.** The ADSP has never run under AsteroidOS, while stock ran it all the time. Measure idle drain before and after. PulseAudio's `module-suspend-on-idle` closes the device when idle.
- **Scope.** Music plays on the iPhone through ioslink's music bridge, so this fix is about local sounds: notifications, alarms, timers, and voice input.

Research notes, raw evidence and scripts (configs, device tree, stock modules, checksum tables, `crc_compare.py`) are in `~/.cache/sonic-screwdriver/research-sound/`. Start from `NOTES.md` there.
