# Sound fix

Make the speaker and the two microphones work. Audio had never worked on this port; AsteroidOS rates both "Bad" for rubyfish.

Status (2026-10-30):

- **The kernel side works.** This branch adds five kernel commits, listed [below](#the-fix). With them, Mobvoi's 25 stock audio modules load from the vendor partition (all 493 symbol CRCs they check match, as on the stock kernel), the ADSP boots, the sound card and the amplifier come up, and `aplay`, `arecord` and PulseAudio work through ALSA. Trial build 022 (`#22-13ebf00`, brightness and touch fixes included) runs from `recovery`; `13ebf00` differs from this branch only in `todos/`.
- **The speaker works through Mobvoi's own playback path,** the vendor audio HAL (`audio.primary.rover.so`), loaded in-process with libhybris: 0.1 s 1 kHz blips from -40 to -19.5 dBFS, all heard. That path also runs Awinic's speaker protection on the ADSP. The microphone records.
- **Plain ALSA went silent at 17:03 on 2026-10-30, and plays again once the HAL has played on the same boot.** It played at 17:01–17:03 on a fresh boot, stopped, stayed silent after a reboot, and played again at 19:35 after the HAL had played on build 021's boot. Earlier I took the silence for a broken speaker; the HAL showed it isn't. See [Plain ALSA went silent](#plain-alsa-went-silent).
- **The kernel caps the speaker's loudness** at the user's limit: whatever writes to the amplifier, its volume stays at least 20 dB below full gain, so full scale on any path lands within about a dB of -20 dBFS through the HAL. The user chose 20 dB after hearing full scale through a 22 dB cap ("still not too loud"), and heard full scale through it as "the right ceiling". See [Loudness cap](#loudness-cap).
- **Rule: ask the user before playing any sound on the watch.** Test with 0.1 s blips. Nothing may play louder than -20 dBFS through the HAL.
- **Not set up permanently** on the watch: no boot unit, no HAL service, no PulseAudio configuration, nothing that plays sound by itself. See [What remains](#what-remains).

## The user's limit

On 2026-10-30 the user judged 0.1 s 1 kHz blips through the vendor HAL (the 1 kHz triangle-shaped blip `haltone` plays, see [By ear: the vendor HAL](#by-ear-the-vendor-hal)):

| Level | Verdict |
|---|---|
| -40 dBFS | quiet |
| -35 dBFS | acceptable |
| -33, -31 dBFS | good |
| -30 dBFS (Mobvoi's 1% step) | good, not too loud |
| -25, -22 dBFS | medium |
| -19.5 dBFS (Mobvoi's 35%) | starts to be high: the max |

**-20 dBFS through the HAL is the most this speaker may ever play.** That is about 33–35% on Mobvoi's speaker volume curve for notifications, rings and alarms (`/vendor/etc/audio_policy_volumes.xml`: 1% -29.7 dB, 33% -20.1 dB, 66% -10.2 dB, 100% 0 dB). The kernel cap enforces it for every path; a persistent setup must still keep its own levels well inside it.

## What was wrong

The diagnosis of 2026-10-29 held up, and two more problems turned up once the sound card existed.

1. **The drivers aren't in the kernel source.** All of Qualcomm's audio drivers, and Mobvoi's code for this watch's amplifier, exist only as 25 binary modules on the vendor partition, `/vendor/lib/modules/audio_*.ko`. Stock Wear OS loads them at boot. No public source has Mobvoi's amplifier glue.
2. **The AsteroidOS kernel didn't fit those modules.** AsteroidOS's `0001-Disable-tracing.patch` and its defconfig turned tracing off, because the GPU and IPA trace headers broke its build. That changes `struct module` and `struct task_struct`, and removes two exports. Of the 493 CRCs the modules check, 343 differed and 2 symbols were missing (`kmem_cache_alloc_trace`, `kmalloc_order_trace`, exported only with `CONFIG_TRACING`): nothing loaded. Module signing, also off, adds a `sig_ok` field to `struct module`.
3. **alsa-lib couldn't talk to this kernel.** AsteroidOS's 32-bit user space, alsa-lib 1.2.13 included, is built with a 64-bit `time_t` (it imports `__ioctl_time64`). `struct snd_pcm_sync_ptr` and `struct snd_pcm_status` hold timespecs, so their size, and with it the number of the `SYNC_PTR`, `STATUS` and `STATUS_EXT` ioctls, changes with `time_t`. Kernels learned the 64-bit layouts in 5.6; 4.9 answered `-ENOTTY`, so every PCM open through alsa-lib failed: "audio open error: Inappropriate ioctl for device". Any 32-bit AsteroidOS with a 64-bit `time_t` on a kernel older than 5.6 has this problem. Android's tinyalsa, built with a 32-bit `time_t`, isn't affected, which is why the HAL never ran into it.
4. **Nothing loaded the modules or booted the ADSP.** AsteroidOS runs Android's init with its own `init.rc` and never reads the vendor's `init.target.rc` (`modprobe` of the 25 modules) or `init.qcom.rc` (`write /sys/kernel/boot_adsp/boot 1`).
5. **PulseAudio had no real output,** only `auto_null`: meta-rubyfish removed the droid module, and nothing loads the ALSA modules. `/etc/asteroid/machine.conf` doesn't declare a speaker either.
6. **Nothing limits the loudness.** Mobvoi's amplifier driver sets the amplifier's own volume to 0 dB, its maximum, at every stream start, and the amplifier boosts its supply well above the battery's. A full-scale signal from any player is far louder than the user's limit, and on Wear OS only the HAL's volume curves kept it down.

## The fix

| Commit | What |
|---|---|
| `140ae2203df7` | **msm: kgsl, ipa: keep the tracepoints compiled out with NOTRACE.** "Disable tracing" dropped the tracepoint definitions of the GPU (KGSL/Adreno) and IPA drivers; `ccflags-y += -DNOTRACE` makes their `trace_*()` calls empty stubs whatever the config. A no-op with tracing off: the kernel Image is byte-identical to its parent's. |
| `6e4f1260c8da` | **ARM: configs: asteroidos-rubyfish: turn tracing back on as in stock.** The tracing options exactly as in the stock kernel's config: `FTRACE` with syscall tracing and the preempt/irq events, no function or latency tracers. |
| `b7abd4115f03` | **ARM: configs: asteroidos-rubyfish: check module signatures without enforcing them.** `MODULE_SIG` and SHA-512 as in stock, without `MODULE_SIG_FORCE` and `MODULE_SIG_ALL`. The stock modules are signed with Mobvoi's throwaway build key, which this kernel doesn't have, so they load as unsigned modules and taint the kernel (`E`). The CRC check stays on. |
| `65cc7e0a3408` | **ALSA: pcm: accept the 64-bit time_t layouts of SYNC_PTR and STATUS on 32-bit kernels.** Linux 5.6's `struct snd_pcm_status64` and `__snd_pcm_sync_ptr64`, handled at the ioctl boundary next to the 32-bit ones, which are unchanged. Additive only, built only for 32-bit kernels; the PCM protocol version stays 2.0.13. |
| `09b04fb99713` | **i2c: cap the loudness of the watch's speaker amplifier.** Every transfer to the amplifier goes through `__i2c_transfer()`, which now keeps its volume at least 20 dB below full gain, see [Loudness cap](#loudness-cap). No exported symbol, structure or signature changes. |

Tools, in `sonic/`: `b720b9d26b71` adds `modcrc.py`, which repeats the CRC check on any build (see [BUILD.md](../sonic/BUILD.md#kernels-with-the-stock-module-abi-sound-and-later)), and `67688aefb87a` adds `kernel-trial-wifi.service`, which reconnects the saved Wi-Fi network when a trial boots without it.

The config now differs from stock only in AsteroidOS's own choices (Bluetooth, devtmpfs, tmpfs ACLs, USB NCM, the command line, `LOCALVERSION`, `MODULE_FORCE_LOAD`, `PANIC_ON_OOPS`) and in `MODULE_SIG_FORCE`/`MODULE_SIG_ALL`. `LOCALVERSION` stays empty, so `uname -r` is still `4.9.232`; the modules' `4.9.232-perf+` doesn't matter, because `same_magic()` skips the release when CRCs are present.

## Loudness cap

### Why in the kernel

The amplifier is an Awinic AW882xx (chip ID `0x1852`) at `0x34` on I2C bus 2. Its driver is Mobvoi's closed `audio_AW882xx.ko`, and anything running as root can also reach the chip through `/dev/i2c-2` or the driver's `reg` and `rw` sysfs files. A volume limit in PulseAudio or a HAL bridge would cover only that one player. The I2C core is the one place every path goes through, and it is in our source:

- The vendor driver writes registers with `i2c_master_send(client, [reg, value >> 8, value & 0xff], 3)` and reads with `i2c_smbus_write_byte(client, reg)` and `i2c_master_recv(client, buf, 2)` (from its disassembly: these are its only I2C imports, no regmap). Its sysfs files, its misc-device ioctl, its ALSA controls and its monitor all use them.
- `i2c_master_send()`/`i2c_master_recv()`, `i2c_transfer()` and `/dev/i2c-*` (`read`, `write`, `I2C_RDWR`) all end in `__i2c_transfer()`. SMBus calls do too, emulated, because bus 2's controller driver (`i2c-msm-v2`) has no native SMBus.
- `CONFIG_DEVMEM` and `CONFIG_KPROBES` are off.

### What it does

`drivers/i2c/i2c-speaker-cap.c`, called by `__i2c_transfer()` before the bus driver sees the messages, for adapter 2, address `0x34` only:

- **Volume.** `HAGCCFG4` (`0x0c`), bits 15:8 (`VOL`): attenuation = 6 dB × `VOL[7:4]` + 0.5 dB × `VOL[3:0]`, from the 0x1852 register map in Awinic's public driver; `0x00` is 0 dB (the factory setting), `0xf0` is the driver's mute. A write that asks for less attenuation than the cap goes out with the cap (`0x34` = 20.0 dB) and the rest of the register unchanged. More attenuation (fades, mute) goes out as written.
- **Speaker gain.** `SYSCTRL` (`0x04`), `SPK_GAIN` bits 14:12 (factory 4, AV16; 5 would be AV20, +1.9 dB): never above 4.
- **After a reset.** Every reset (the driver's reset GPIO, a soft reset, power-up) sets `VOL` back to 0 dB, but also powers the amplifier down (`SYSCTRL` `0x4003`), and only a write to `SYSCTRL` powers it up again. So before any write to `SYSCTRL` goes out, the cap reads `VOL` back from the chip and sets it to the cap if it is louder. Mobvoi's factory register file, which the driver loads at the first stream, even writes `SYSCTRL` before `VOL`.
- **Refused** with `-EPERM`: transfers the check can't follow. More than one register write to the amplifier in one transfer (a reset could sit between the volume check and the power-up), a write of more than one register in one message (whether the chip auto-increments is undocumented), and `I2C_M_NOSTART` or `I2C_M_REV_DIR_ADDR` (the bytes would no longer start with the register address). The vendor driver never sends any of these.
- The caller's buffers are never modified (they may be `const`): a clamped write goes out from a copy, and the caller's buffer pointer is put back after the transfer.
- `/sys/kernel/speaker_cap/` shows `attenuation_db` (20.0) and counts the writes `clamped` and `refused` since boot. The first clamp and every refusal (rate-limited) are logged. The cap is a compile-time constant, `SPKCAP_MIN_ATTEN` (0.5 dB steps), not a module parameter: built-in parameters can be set on the kernel command line.

Not touched, on purpose: the boost converter's output voltage (it sets the supply rail, so it only limits the swing; the class-D gain is `SPK_GAIN`, and with the cap the output stays far below the rail), the chip's hardware AGC (it can only lower the gain, is off in the factory settings, and its thresholds are undocumented), the I2S input level and channel selection (they can only attenuate or select), and receiver mode (lower gain).

### The numbers

The amplifier measures its own output voltage (V-sense) and current (I-sense) and streams both on its I2S output, the feedback Awinic's algorithm uses. Recorded through `MultiMedia2 Mixer QUAT_MI2S_TX` (`arecord -D hw:0,1 -c 2`, channel 0 = I, 1 = V), at the factory volume (0 dB), the 1 kHz amplitudes were:

| Path, input | V-sense (counts) | Source |
|---|---|---|
| HAL, -30 dBFS, first 1.0 s | 857.3 | 5 s tone, 18:18:09; then exactly 6.0 dB lower (427.6) for the rest, from something on the ADSP: the amplifier's volume hadn't changed |
| HAL, -40 dBFS | at least 294 | register samples (`VSNDAT`) of a 5 s tone, 18:14:28 |
| plain ALSA, -20 dBFS | 3049.7 | 3 s tone, 17:56:37, steady to ±2 |

- Scaled linearly, **-20 dBFS through the HAL gives 2711 counts**, or about 2500 if the HAL path compresses a little above -30 dBFS, as the -40 dBFS samples hint. Full scale through plain ALSA, i.e. a full-scale I2S signal if plain ALSA's default topology passes samples unchanged, **gives about 30,500 counts** (93% of the sense range). The HAL path comes out about 1 dB below plain ALSA at the same level.
- So a full-scale signal is 21.0 to 21.7 dB louder than the user's limit. The cap was first the chip's next step above that, 22.0 dB (builds 020 and 021). Through it, a -40 dBFS blip through the HAL measured 0.8 to 1.5 dB quieter still than predicted, and the user heard full scale, through the HAL and through plain ALSA, as "still not too loud". **The user then chose 20.0 dB** (build 022): full scale on any path lands within about a dB of -20 dBFS through the HAL.

### Verification

At the user's request, the checks were silent or at the user's "quiet" until the cap was in and verified; only then did the user listen to full scale through it.

- **Unit test**, against a simulated chip that checks after every write that whenever it is powered up, its volume is at least the cap and its speaker gain at most 4: a driver-like cold start (factory register file, fade-in, the monitor's 0 dB rewrite), a soft reset followed by a power-up with no volume write, every possible `VOL` and `SPK_GAIN` value, the refusals, the caller's buffers. No violation.
- **Silent register test**, with no PCM open and the amplifier powered down, on build 021 (22.0 dB) and again on build 022 (20.0 dB): `VOL` 1.5 dB, then 1 dB, louder than the cap, written through the driver's `rw` file, `write()`, `I2C_RDWR` and an SMBus word on `/dev/i2c-2`, reads back the cap every time; a mute value is kept; `SPK_GAIN` 5 through `rw`, `write()` and SMBus reads back 4; a 5-byte write, two writes in one transfer and `I2C_M_NOSTART` return `EPERM` and change nothing.
- **One capped blip,** on build 021: -40 dBFS through the HAL (the user's "quiet"), 19:26:34. At the stream start the driver's factory volume of 0 dB was clamped to 22.0 dB. V-sense: 19.6 counts, against 21.5 to 23.4 predicted from the table above: 0.8 to 1.5 dB quieter than predicted. The predictions come from long tones, not same-shape blips, and 20 counts is the very bottom of the sense range.
- **By ear, with the 22.0 dB cap,** at the user's request (19:32–19:37, build 021): through the HAL, 0.1 s 1 kHz blips at -40 dBFS (not heard), -30 dBFS ("very faint"), -20 dBFS and 0 dBFS, full scale; through plain ALSA, a 0.6 s voice clip at -3.7 dBFS peak (heard), then the same clip and its full 1.7 s at full scale. Full scale, through either path, was "still not too loud". The volume register read 22.0 dB before and after each. The driver writes the volume only at the first stream after the modules load; later streams leave the clamped value (`clamped` stayed at 9).
- **By ear, with the 20.0 dB cap** (build 022, 19:53–19:55): after a -30 dBFS HAL blip, the full 1.7 s voice clip through plain `aplay` at full scale (heard), at Mobvoi's 80% step (-6 dB), and at full scale again: "the right ceiling. I think we can go higher, but let's keep it at that." **The cap stays at 20.0 dB.** The volume register read 20.0 dB before and after every sound.
- Builds 020 (24 dB, never flashed), 021 (22 dB) and 022 (20 dB): `verify.py` passes, `modcrc.py` gives 493 match, 0 mismatch, 0 missing, `Module.symvers` and the build warnings are the same as build 019's. The fake-amplifier unit test passes at each cap.

### Limits

- The cap protects against user space, not against kernel code: root can still load a module of its own (module signatures aren't enforced, see above) or flash another kernel.
- It holds for this chip at this address. Bus 2 and `0x34` are compiled in, from `rover-audio.dtsi`, which both rover and rubyfish use.
- The cap doesn't limit how long the speaker plays. Awinic's algorithm on the ADSP, which only the HAL path runs, is what protects the coil from heat.

## Tests (2026-10-30)

### Offline: symbol CRCs

`sonic/modcrc.py` reads the export tables out of a kernel image and the `__versions` table of every module, the way `kernel/module.c` checks them at load time. The stock modules check 492 kernel symbols plus `module_layout`; another 395 symbols they resolve between themselves.

| Kernel | Match | Mismatch | Missing |
|---|---|---|---|
| Stock Wear OS (`stock-backup/boot.img`) | 493 | 0 | 0 |
| AsteroidOS 2.1 release | 148 | 343 | 2 |
| tracing only (`6e4f1260`, build 017) | 239 | 254 | 0 |
| this branch (builds 018 to 022) | **493** | **0** | **0** |

Per (module, symbol) entry: 1482 match, none fails. All 9211 symbols this kernel exports have the stock kernel's CRCs; it lacks 20 that stock exports, the 4 KGSL tracepoints (compiled out) and 16 USB NCM helpers (off in AsteroidOS). `sizeof(struct module)`, from `.gnu.linkonce.this_module`: 512 bytes, as in the stock modules (448 on the 2.1 release). No stock module exports a symbol the kernel already exports. The research script `crc_compare.py` gives the same numbers.

With tracing on, GCC 14.3 builds the kernel without errors. The only new warnings are a `-Warray-compare` on the `trace_printk` section bounds in `kernel/trace/trace.c` and OpenSSL 3 deprecations in the host tool `scripts/sign-file.c`; none in the GPU, IPA, ALSA or I2C code.

### Trial boots from `recovery`

- **Builds 018** (the three config/NOTRACE commits), **019** (plus the ALSA commit), **021** and **022** (plus the cap, at 22.0 and 20.0 dB), each booted from `recovery` following `sonic/BUILD.md`: `trial-check.sh` healthy (Wi-Fi, Bluetooth, ioslink, display, touch, launcher, no new oops or WARN), `kmem_cache_alloc_trace` and `kmalloc_order_trace` in `/proc/kallsyms`.
- **Kernel log**, first 90 s, against a normal boot: new only the `trace_printk() being used` NOTICE banner (Qualcomm's `msm_bus_fabric_adhoc.c` calls `trace_printk()`, as in stock; the trace buffer then takes 1411 kB per CPU, 5.6 MB in all), `Loaded X.509 cert 'Build time autogenerated kernel key: …'`, systemd mounting the trace file system, and on 021 and 022 the cap's `i2c-speaker-cap: speaker amplifier 2-0034 held at least 20.0 dB below full gain` (22.0 on 021).
- **021's first boot** couldn't be reached: the access point rejected the first Wi-Fi association, connman didn't retry, and the watch was off its cable. The dead-man's switch took it back to the normal kernel after 10 minutes, as designed; its journal shows a clean boot. That is what `kernel-trial-wifi.service` is for. The second boot was healthy and confirmed, and so was 022's first.
- **Touch and display:** my button wake, taps and swipes through several screens respond normally; emulated wakes re-initialise the touch controller, with no `Runtime PM-callback` or I2C timeout.

### Bringing audio up by hand

- **Modules:** all 25 `insmod` in stock's order, in about 1 s. Each logs `PKCS#7 signature not signed with a trusted key`; the first adds `module verification failed: signature and/or required key missing - tainting kernel`. No `Unknown symbol`, no `disagrees about version`.
- **Amplifier:** `aw882xx driver version v0.1.6`, `aw882xx detected`, chip ID `0x1852`, `probe completed successfully!`, `2-0034` bound to `aw882xx_smartpa`. It runs without its interrupt (`skipping IRQ registration`), as in stock.
- **ADSP:** `echo 1 > /sys/kernel/boot_adsp/boot` loads `adsp.mdt` through `firmwared` (no `firmware_class.path` needed), out of reset after 0.3 s, `apr_tal:Q6 Is Up`, `subsys1` `ONLINE` after 2 s. The sound card driver, deferred until then, registers `sdm429w-snd-card`: 32 PCM devices, 2786 mixer controls. Device 0 is MultiMedia1, playback and capture.
- **First playback after the modules load:** the amplifier cold-starts, loads `aw882xx_spk_reg.bin` and reads the factory calibration from `/persist/aw_cali.bin` (`get_cali_re_from_file: 39041`, about 9.5 Ω). It stops when the stream closes.
- **Wakeup sources:** none of the audio ones (`pil-adsp`, `adsp_IPCRTR`, `apr_apps2`, …) stays active when idle.

### ALSA, PulseAudio and the time64 fix

- Mixer routes, from stock's `mixer_paths_sdm429w.xml` ("deep-buffer-playback", "audio-record", "dmic0"): `QUAT_MI2S_RX Audio Mixer MultiMedia1` = 1 for the speaker; `DEC1 MUX` = `DMIC1` and `MultiMedia1 Mixer TERT_MI2S_TX` = 1 for the microphone. **`aplay` needs the speaker route set first** (`amixer -c0 cset name='QUAT_MI2S_RX Audio Mixer MultiMedia1' 1`): it is off after boot, and the HAL turns it off again when its stream closes. Without it, `aplay` fails with "Unable to install hw params": the front end (MultiMedia1) has no back end. The stock "speaker" path's `mono Profile`/`mono Music Playback Volume` are TFA98xx controls that don't exist with this amplifier.
- **Build 018:** `aplay -D hw:0,0` fails, "audio open error: Inappropriate ioctl for device" (problem 3). A static tinyalsa built with a 32-bit `time_t` played and recorded, which proved the audio path before the kernel fix.
- **Build 019:** `aplay -D hw:0,0` plays (48 kHz, 125 ms periods, no underrun) and `arecord -D hw:0,0 -c 1` records DMIC1.
- **Proof that the three ioctls are all that's needed:** the kernel's syscall trace events (tracing is on now) record every ioctl and its result. `arecord`: every PCM ioctl succeeds, `SYNC_PTR64` 12 times. PulseAudio loading `module-alsa-sink`/`module-alsa-source` on `hw:0,0` and playing: `SYNC_PTR64` and `STATUS_EXT64` from its I/O threads, `REWIND`, `CHANNEL_INFO`, all succeeding, and no ALSA timer ioctl at all (its timer-based scheduling uses its own timers). A time64 alsa-lib first tries to mmap the status and control records at the offsets Linux 5.6 added for the new layout (`SNDRV_PCM_MMAP_OFFSET_STATUS_NEW`, `_CONTROL_NEW`); 4.9 doesn't know them, the mmap fails at open, and alsa-lib falls back to `SYNC_PTR`, on any architecture (on ARM even the old offsets are refused). Checked against alsa-lib 1.2.13's source and its own header: the patch's structures match it field by field.
- **PulseAudio** played through `module-alsa-sink` and closed the device 5 s after the sound (`module-suspend-on-idle`), but heard by nobody: AsteroidOS's main volume (`MainVolume2`) was at step 0 of 20, which mutes every application stream. PulseAudio also logs "ALSA woke us up to write new data to the device, but there was actually nothing to write" now and then: the quirk of Qualcomm's PCM driver below.

### By ear: plain ALSA

| Time | What | Heard? |
|---|---|---|
| 16:48 (build 018) | tinyalsa, -40 dBFS tone, underrunning every 20–40 ms | yes: "blblbl", choppy |
| 16:51–16:59 (018) | tinyalsa with no stop threshold, tones and a clip | no (cause not established) |
| 17:01:58 (019) | `aplay`, 1 kHz, -40 dBFS, 3 s (amplifier cold start) | yes, clean, comfortable |
| 17:03:29, 17:03:35 | `aplay`, 1 kHz, -30 and -20 dBFS, 3 s each | yes, clean, comfortable |
| 17:03:41 | `aplay`, notification chime, -12 dBFS peak, 0.25 s | **no** |
| 17:04:08 | `aplay`, a recording boosted by 40 dB: broadband noise and typing, -9 dBFS peak, 6 s | unclear |
| 17:19 on, 36 attempts | `aplay`, `tinyplay`, PulseAudio; -30 to -12 dBFS; tones, sweeps, chimes; four front ends; 16/24-bit, 44.1/48 kHz, mono/stereo; speaker and receiver modes; after a reboot too | **no** |
| 19:35:46 (build 021, cap 22.0 dB, after the HAL had played) | `aplay`, a 0.6 s voice clip, -3.7 dBFS peak | **yes** |
| 19:36–19:37 (021) | the same clip, and its full 1.7 s, at full scale | yes: "still not too loud" |
| 19:54:02 (022, cap 20.0 dB, fresh boot, after one HAL blip) | `aplay`, speaker route not set | fails: "Unable to install hw params" |
| 19:54:36–19:55:13 (022) | route set; the 1.7 s clip at full scale, at -6 dB, at full scale again | yes: "the right ceiling" |

### By ear: the vendor HAL

- **The HAL loads on AsteroidOS as it is.** The watch mounts the stock system partition at `/android-root` (`/system` with its VNDK libraries) and the vendor partition, and has libhybris. A small client, `haltone` (not in this repository; in the research files below), `dlopen()`s `libhardware.so.2`, gets the `audio`/`primary` module (`audio.primary.rover.so`, device API 2.0), opens a speaker output stream (`PRIMARY|DEEP_BUFFER`, 48 kHz stereo 16-bit) and writes a 1 kHz tone: `haltone DBFS SECONDS HZ LEAD TAIL`, e.g. `haltone -40 0.1 1000 0.5 1.5`. Each tone fades in and out over 50 ms, so a 0.1 s blip is a triangle that reaches the level at its peak.
- **What the HAL does that plain ALSA doesn't:** it loads the calibration database (all 8 `MTP_SDM429W_*.acdb` files, "Common custom topology in use"), applies the "speaker" and "deep-buffer-playback" mixer paths (speaker ACDB ID 14, app type `0x11131`, AFE topology `0x1000ff01`), starts the amplifier's current/voltage feedback on `QUAT_MI2S_TX` (`aw882xx_start_feedback`, ACDB ID 102, AFE topology `0x1000ff00`), and switches Awinic's algorithm on: `aw882xx_rx_switch` and `aw882xx_tx_switch` go to 1, and reading them works (the ADSP now has the module). Qualcomm's own speaker protection stays off (`Speaker protection disabled`).
- **Heard:** 0.1 s 1 kHz blips from -40 to -19.5 dBFS, all of them; the user's verdicts are in [The user's limit](#the-users-limit). The amplifier's current sense carries the tone: current flows.
- **The level steps down by exactly 6.0 dB after 1 s** of a continuous tone through the HAL (-30 dBFS: 857 → 428 V-sense counts). The amplifier's volume register still reads 0 dB 2.5 s into a HAL tone, so it is something on the ADSP, maybe the algorithm settling. Short sounds play at the first level.

## Plain ALSA went silent

Plain ALSA played at 17:01–17:03, then stopped at 17:03:41, and stayed silent after a reboot. The speaker is fine: the HAL plays through it at every level. What is known:

- **It plays again after the HAL.** On build 021's boot, after the HAL had played (19:26–19:34), a 0.6 s voice clip through plain `aplay -D hw:0,0` was heard (19:35:46); on build 022's, after one HAL blip, the full clip was heard too (19:54:36). So something the HAL sets up, and that stays set until the next boot, is what the plain path was missing: likely the speaker port's ADSP topology and app type (`App Type Cfg`), and Awinic's module switched on (`aw882xx_rx_switch`, `aw882xx_tx_switch`). Why plain ALSA played without it at 17:01–17:03, on a fresh boot before the HAL had ever run, is open.
- **The amplifier is the same in both paths.** During playback every register reads the same through plain ALSA and through the HAL, except the battery, temperature and sense readings (`SYSST` `0x0311`: PLL locked, clocks stable, boost on, output switching, no fault; `SYSCTRL` `0x4440`; `HAGCCFG4` `0x0064`), and equals the factory register file except the status, sense and calibration registers.
- **The voltage sense carries the tone in both,** at matching levels: plain ALSA about 1 dB above the HAL for the same input.
- **The current sense doesn't, in plain ALSA:** the 1 kHz line is missing there (0.2 counts against 3050 on the voltage channel at -20 dBFS), while through the HAL it carries the tone (386 counts at -30 dBFS). Taken at face value, plain ALSA drives a voltage that makes no current. But the HAL also configures the feedback port and the Awinic module before it records the feedback, and plain `arecord` doesn't, so the two recordings may not decode the sense slots the same way.
- **Not the cause:** capture running at the same time (17:30), PulseAudio's state (silent with its ALSA modules unloaded, and after a reboot). Nothing persistent changed (the calibration file is from 2021). The batch at 17:50 that varied the front end, the sample format and rate and the player ran in receiver mode by mistake, so those remain untested.

Not investigated further: a persistent setup should play through the HAL anyway, for the protection. The microphone works through plain ALSA.

## Speaker protection

The AW882xx is a "smart" amplifier: it boosts its supply well above the battery voltage and relies on an algorithm on the ADSP to keep the speaker's excursion and coil temperature safe. The algorithm reads the amplifier's current and voltage sense data and limits the drive.

- **Stock Wear OS and the HAL here:** the audio HAL (`audio.primary.rover.so`) loads the calibration database (`/vendor/etc/acdbdata/MTP_SDM429W_*.acdb` through `libacdbloader.so`). The speaker calibration file contains Awinic's ADSP IDs: `0x10013D00` and `0x10013D01`, and `0x10013D11`, the parameter that switches the algorithm on in Awinic's public driver (`AW_MSG_ID_RX_SET_ENABLE`). For speaker playback the HAL starts the feedback capture on `QUAT_MI2S_TX` (Mobvoi's `audio_extn_aw882xx_start_feedback`; `audio_platform_info.xml`: `SND_DEVICE_IN_CAPTURE_VI_FEEDBACK` on `QUAT_MI2S_TX`, ACDB ID 102; protected speaker ACDB ID 136). The amplifier driver turns the algorithm on (`aw882xx_rx_switch`, `aw882xx_tx_switch`) and sends it the factory calibration (`aw_cali.bin`) at every start.
- **Plain ALSA:** no calibration database is loaded, so the ADSP runs a default topology without the Awinic module; reading `aw882xx_rx_switch` or `aw882xx_tx_switch` returns `ADSP_EBADPARAM`. Nothing protects the coil.
- **So play the speaker only through the HAL.** The kernel cap limits the level on every path, but only the HAL path protects the coil over time.

## Userspace

Prepared but **not installed** (2026-10-30):

- **At boot,** a systemd oneshot unit, `sonic-audio.service`, would run the `insmod` loop, boot the ADSP, wait for the sound card and set the mixer routes. It does nothing unless `/proc/kallsyms` has `kmem_cache_alloc_trace`, so a rollback to the release kernel is unaffected, and it logs and stops at the first failed `insmod` without failing the boot. Ordered `RequiresMountsFor=/vendor/lib/modules /vendor/firmware_mnt/image`, `After=android-init.service firmwared.service`, `Before=user@1000.service`, so the card exists before PulseAudio starts.
- **PulseAudio:** `~ceres/.config/pulse/default.pa` would include `/etc/pulse/default.pa`, then, under `.nofail`, load `module-alsa-sink` and `module-alsa-source` on `hw:0,0` (48 kHz; the source mono) and make them the defaults. For the speaker, plain ALSA plays only after the HAL has on the same boot, and without Awinic's protection: use the HAL route (below). The source is useful as it is.
- **`HAS_SPEAKER = true`** under `[Capabilities]` in `/etc/asteroid/machine.conf`.
- **Installed:** `alsa-utils-amixer` and `alsa-utils-aplay` 1.2.13 (from the 2.0 feed the watch uses).

Found on the way, for whoever sets this up:

- **Notifications make no sound on AsteroidOS 2.1 anyway.** Its ngfd events (`notif_normal`, `notif_strong`, `alarm`, `ringtone`) only vibrate; none names a sound file. ioslink asks for `notif_normal`. A sound needs an event override in `/etc/ngfd/events.d/`.
- **The main volume starts muted** (`MainVolume2` `CurrentStep` 0 of 20; each step is 5% of PulseAudio's volume, step 10 = -18 dB).
- **The system suspends during playback:** off the cable, `PM: suspend entry` came in the middle of a 3 s tone (the suspend aborted on a pending wakeup). The stock HAL holds a wakelock while audio plays; here something must.
- **Qualcomm's PCM driver** (`msm-pcm-q6-v2` in `audio_platform`): a write sends at most one DSP buffer, so write one period at a time (`aplay` does); and the hardware pointer runs ahead of real time with small buffers, so with the default stop threshold (one buffer) a stream underruns every 20–40 ms. PulseAudio and the HAL set the threshold to the boundary.

## What remains

- **A persistent setup, under the user's limit:**
  1. Load the modules and boot the ADSP at boot (`sonic-audio.service`, above).
  2. **A HAL service:** play through the vendor HAL, not plain ALSA, so the calibration and Awinic's protection run. Either PulseAudio's droid module (`pulseaudio-modules-droid-jb2q`, which meta-rubyfish's `pulseaudio_%.bbappend` removes, [4933fa7](https://github.com/AsteroidOS/meta-smartwatch/commit/4933fa7dc0), because PulseAudio kept crashing; probably with beluga's `quirks=+unload_call_exit`; needs an AsteroidOS build, the package is in no feed this watch can reach), or a small bridge like `haltone`: a daemon that opens the HAL's speaker stream while PulseAudio has something to play.
  3. **A PulseAudio bridge** to it, as the default sink, with volumes that keep full scale well inside -20 dBFS through the HAL: the kernel cap is the backstop, not the volume control.
  4. **A keepalive wakelock** while sound plays (PulseAudio's keepalive module, or the bridge holding a wakelock), so the watch doesn't suspend mid-sound off the cable.
  5. Then `HAS_SPEAKER`, and ngfd sound events if wanted.
- **Ask the user before any sound,** and keep tests to 0.1 s blips, until the setup is done and the user has heard its levels.
- **Plain ALSA's silence:** it plays again once the HAL has played on the same boot; what the HAL sets up that it was missing is still open (above). Not needed for the speaker if the HAL route is used.
- **Upstream (meta-smartwatch):** the defconfig, the reworked `0001-Disable-tracing.patch` (`-DNOTRACE`), the ALSA patch (any 32-bit AsteroidOS on a pre-5.6 kernel needs it for alsa-lib), the loudness cap or an equivalent, `insmod` and `write /sys/kernel/boot_adsp/boot 1` in meta-rubyfish's `init.rc` (as meta-beluga does), and the HAL route. `MACHINE_HAS_SPEAKER = "true"` only once that works. No vendor files need redistributing: the modules come from the watch's own vendor partition.
- **Microphone gain:** DMIC1 works, but speech at 20 cm peaks only around -50 dBFS. Stock gets its gain from the calibration database too. `DEC1 Volume` (84 = 0 dB, up to +40 dB) is the plain-ALSA knob. DMIC2 is untested.
- **Seven modules serve the disabled tasha card** and could be dropped: `audio_usf`, `audio_swr_ctrl`, `audio_hdmi`, `audio_wcd_cpe`, `audio_cpe_lsm`, `audio_wcd9335`, `audio_machine_ext`.
- **The three rootfs modules** (`br_netfilter`, `rdbg`, `lcd` in `/lib/modules/4.9.232`) were built for the 2.1 release and don't load on this kernel. Nothing loads them; an AsteroidOS build ships matching ones.
- **Memory:** with tracing on, 2.4 MB less is available at boot (800412K instead of 802824K of 923648K), and the trace buffer takes another 5.6 MB (`echo 1 > /sys/kernel/tracing/buffer_size_kb` would free most of it at run time). Stock pays the same.
- **`MODULE_SIG` without enforcement** checks nothing it can trust: the stock modules' signer isn't known to this kernel. AsteroidOS had module signing off entirely before.

## Reference

### Hardware path

| Part | What it is | Evidence |
|---|---|---|
| Codec | The SDM429W's internal **digital** codec only. There is no analog codec, so no headset. | Device tree: `msm_dig_codec` @ `c0f0000`, `qcom,no-analog-codec` |
| Microphones | **Two digital mics**, DMIC1 and DMIC2. Each feeds the codec's decimator (DEC1/DEC2), which sends audio to the DSP over the tertiary I2S link (`TERT_MI2S_TX`). Stock uses DMIC1 for recording. | Sound node routing `DMIC1 → Digital Mic1`, `DMIC2 → Digital Mic2`. Stock `mixer_paths_sdm429w.xml`: `DEC1 MUX = DMIC1`, `MultiMedia1 Mixer TERT_MI2S_TX = 1` |
| Speaker amp | **Awinic AW882xx** smart amplifier, chip ID `0x1852`, on I2C bus 2, address `0x34`. Audio in on the quaternary I2S link (`QUAT_MI2S_RX`); current/voltage sense out on `QUAT_MI2S_TX`. | DT `aw882xx_smartpa@34` (`awinic,aw882xx_smartpa`, reset GPIO 92, IRQ GPIO 93) under `i2c@78b6000`. Sound node: `qcom,msm-ext-pa = "quaternary"`, flag `ext_pa_aw882xx` |
| NXP TFA98xx | An alternative amplifier footprint, **disabled**. The kernel still builds a `tfa98xx` driver (log: `TFA98XX driver version v6.6.3`), which does no harm. | DT `nxp_smartpa@34`, status disabled |
| ADSP | The audio DSP (LPASS/QDSP6) does all mixing and routing, and the speaker protection. **No sound is possible until it boots.** | DT: `qcom,lpass@c200000` (firmware `adsp`); `qcom,msm-adsp-loader` with `qcom,adsp-state = <0>`, meaning userspace must start it; `qcom,msm-audio-apr` |
| "BG" co-processor | **Not involved.** "BG" in the model string comes from Qualcomm's reference design. All BG nodes are disabled and their drivers aren't built. | DT: `qcom,bg-daemon`, `qcom,bg-rsb`, `qcom,glink-bgcom-xprt-bg` all `status = "disabled"` |

The sound card node is `c051000.sound`, with `compatible = "qcom,msm8952-dig-asoc-snd"` and `qcom,model = "sdm429w-snd-card"`. No device-tree change was needed.

### Amplifier registers used here

From the 0x1852 register map in Awinic's public driver (`aw882xx_pid_1852_reg.h`) and Mobvoi's factory register file `/vendor/firmware/aw882xx_spk_reg.bin` (36 register/value pairs, little-endian 16-bit). On the wire: 8-bit register address, 16-bit value, high byte first. The driver's `rw` file reads with `echo REG > rw; cat rw` and writes with `echo "REG VALUE" > rw`.

| Register | Factory | Fields |
|---|---|---|
| `0x00` ID | `0x1852` | chip ID; writing `0x55aa` resets the chip |
| `0x01` SYSST | `0x0311` playing | `BSTS`, `SWS`, `CLKS`, `PLLS`; fault bits (`OCDS`, `OTHS`, `UVLS`, …) |
| `0x04` SYSCTRL | `0x4440` (`0x4003` after reset) | `SPK_GAIN` 14:12 (4 = AV16), `RCV_GAIN` 11:10, `RCV_MODE` 7, `I2SEN` 6, `AMPPD` 1, `PWDN` 0 |
| `0x05` SYSCTRL2 | `0x0028` | `HAGCE` 6 (hardware AGC), `HMUTE` 4, `BST_IPEAK` 3:0 |
| `0x06` I2SCTRL | `0x1408` | `INPLEV` 13 (0 dB or -6 dB), `CHSEL` 11:10 (left, right, (L+R)/2) |
| `0x07` I2SCFG1 | `0x0361` | `CFSEL` 7:6 = I/V sense data on the I2S output, `I2STXEN` 0 |
| `0x0c` HAGCCFG4 | `0x0064` | `VOL` 15:8 (6 dB × 7:4 + 0.5 dB × 3:0 of attenuation), `HOLDTH` 7:0 |
| `0x57` ISNDAT, `0x58` VSNDAT | | current and voltage sense, one sample |
| `0x61` BSTCTRL2 | `0x6373` | `BST_MODE` 14:12, `VOUT_VREFSET` 5:0 (boost voltage, `0x33` = 9.5 V) |

### How stock Wear OS does it (from `stock-backup/vendor.img`)

1. **Load the modules.** `/vendor/etc/init/hw/init.target.rc`, `on early-init`, runs `modprobe -a -d /vendor/lib/modules` on all 25 `audio_*` modules.
2. **Start the ADSP.** `init.qcom.rc`, `on early-boot`, runs `write /sys/kernel/boot_adsp/boot 1`. The firmware comes from `firmware_class.path=/vendor/firmware_mnt/image` on the stock command line; on AsteroidOS, `firmwared` serves the same directory.
3. **Run the audio HAL.** `vendor.audio-hal-2-0` runs `/vendor/lib/hw/audio.primary.rover.so`, Qualcomm's `hardware/qcom/sdm429w/audio` HAL with Mobvoi's AW882xx feedback extension, configured by `mixer_paths_sdm429w.xml` and `audio_platform_info.xml`, with calibration from `MTP_SDM429W_*.acdb` through `libacdbloader.so`.

| Device | Module | Notes |
|---|---|---|
| `qcom,msm8952-dig-asoc-snd` (the sound card) | `audio_machine` | Has Mobvoi-only code that reads `ext_pa_aw882xx` and links the amplifier (`aw882xx-aif`) into the card |
| `i2c:aw882xx_smartpa` (the amplifier) | `audio_AW882xx` | Loads `/vendor/firmware/aw882xx_spk_reg.bin` and its calibration `/mnt/vendor/persist/aw_cali.bin` (rubyfish's `init.rc` links it to `/persist`) |
| `qcom,adsp-loader` | `audio_adsp_loader` | Creates `/sys/kernel/boot_adsp/boot` |

### Routes not taken

- **Building the drivers from source** ([PR #227](https://github.com/AsteroidOS/meta-smartwatch/pull/227), argosphil, 2023): Google's `kernel/msm-extra` `android-msm-beluga-4.9-pie-wear-mr2` plus Awinic's `aw88230` driver. Dropped as not working; its `msm8952.c` lacks Mobvoi's amplifier glue.
- **yesimxev's "audio fix"** ([commit b168859a](https://github.com/yesimxev/mobvoi-ticwatch-kernel/commit/b168859a372f0d411ce7a4da78c5337ef0a0bab0)), for a Kali NetHunter kernel on Wear OS: `MODVERSIONS` and module signing off. It works there because that kernel keeps stock's tracing; it drops the CRC check this branch relies on.
- **Forcing the load** with tracing off (`modprobe -f`): the modules would run against a different `struct module` and `task_struct`. Not done.
- **A volume limit in userspace only** (PulseAudio, the HAL bridge): it would cover one player, not the driver's own writes, `/dev/i2c-2` or the driver's sysfs files. The kernel cap covers all of them; userspace volumes still set the everyday level below it.

Research notes, raw evidence and scripts are in `~/.cache/sonic-screwdriver/research-sound/` (2026-10-29), `~/.cache/sonic-screwdriver/kernel-sound/` (2026-10-30, including `halclient/haltone.c` and the log of every sound played) and `~/.cache/sonic-screwdriver/kernel-volcap/` (the cap: notes, unit test, register test), on the Mac.
