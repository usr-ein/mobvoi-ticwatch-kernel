# NFC (to do)

Get NFC working on the watch: first reading and writing tags, then maybe card emulation. NFC has never worked on AsteroidOS for this watch; asteroidos.org lists it as "bad".

Status (2026-10-29): researched, nothing changed on the watch yet.

- **No kernel change is needed, and no firmware is needed.** The kernel driver is the same one Wear OS used, and it already probes on every boot.
- **What's missing is all userspace.** Start Mobvoi's Android NFC HAL, and add one plugin to `nfcd` (Sailfish's NFC daemon), which AsteroidOS already ships.
- **The first test needs no build.** The plugin is a prebuilt package in the feed the watch already uses.

## Summary

- **Chip:** an ST (STMicroelectronics) ST54-family part: an ST21NFC controller plus a secure element in one package. It is **not** NXP.
  - The NFC side is on I2C bus 6 at address 0x08.
  - The secure element (eSE) is on SPI bus 1.
- **Recommended route:**
  - Mobvoi's HAL, `android.hardware.nfc@1.1-service.st`, is reached over hwbinder by `nfcd` + `nfcd-binder-plugin`.
  - This is the same recipe AsteroidOS uses on the Fossil Gen 6 (hoki) and the OPPO Watch (beluga), where NFC is rated "partial".
- **Main risk:**
  - The `libncicore` in AsteroidOS 2.0/2.1 (1.1.19), and the one in the nightlies (1.1.30), rejects an NCI 2.0 init response if it lists RF-interface extensions.
  - Upstream fixed this in 1.1.33 (July 2026) for an ST54L, a sibling chip.
  - If this chip does the same, rebuild one small library (step 3.2).
- **Realistic features:**
  - reading and writing NDEF tags;
  - reading UIDs and sending APDUs to cards;
  - later, with a newer nfcd, emulating an NDEF tag or a simple HCE app.
- **Payments: no.** See [Payments](#payments-not-feasible).

## Current state (evidence, 2026-10-29)

- **Kernel config** (`zcat /proc/config.gz`):
  - set: `CONFIG_NFC=y`, `CONFIG_NFC_ST21NFCD=y`, `CONFIG_NFC_ST54SPI=y`;
  - not set: `CONFIG_NFC_NCI`, `CONFIG_NFC_HCI`, `CONFIG_NFC_NQ`.
- **Both drivers probe on every boot.** This is identical in `journalctl -k -b 0` and `-b -1`; the dmesg ring buffer had already rotated:
  ```
  st21nfc_probe : Parse st21nfc DTS ,ret=0
  [dsc]nfc_parse_dt: get reset_gpio[20], irq_gpio[21], polarity_mode[1]
  st21nfc_probe:successfully
  st_clock_select: clk_prepare_enable 0
  st54spi_probe: power GPIO = 71
  st21nfc_register_st54spi_cb
  ```
- **Device nodes:**
  - `/dev/st21nfc` (misc 10,61) and `/dev/gto` (237,0, the eSE), both root-only `0600`.
  - `/sys/class/nfc` exists but is empty. That's expected: ST's driver is a raw character device, not a kernel NFC-subsystem driver.
- **Idle and harmless today:**
  - The NFC interrupt is only requested once a HAL opens the device, and it isn't in `/proc/interrupts`.
  - There were zero NFC wakeups this boot and last boot. For comparison, Wi-Fi caused 1,253 wakeups this boot.
- **What AsteroidOS 2.1 already has:**
  - `nfcd` 1.1.13 (`+git0cdf85c537`) and `nfcd-mce-plugin` 1.0.2.
  - `nfcd.service`, disabled. It runs as root and is configured from `/var/lib/environment/nfcd/*.conf`.
  - The D-Bus policy for `org.sailfishos.nfc.daemon`.
  - `libgbinder` 1.1.14 and `libglibutil` 1.0.62.
  - `/usr/lib/nfcd/plugins/` holds only `mce.so`.
- **What it doesn't have:** `nfcd-binder-plugin`, `libncicore` or `libnciplugin`.
- **The Android side works, minus NFC:**
  - `android-init.service` (hal-droid init) runs servicemanager, hwservicemanager and vndservicemanager.
  - It also runs the HALs listed in `/usr/libexec/hal-droid/system/etc/init/*.rc` and `/init.rc`: Bluetooth, configstore, graphics allocator and composer, and power. GNSS is defined but not running.
  - There's no NFC HAL entry.
  - The HAL binary and its configs are present in `/vendor`, and every library it links against resolves:
    - `/system/lib`, `vndk-28` and `vndk-sp-28`;
    - the hal-droid lib directory;
    - `/vendor/lib`.
  - There is no `nfc` user on AsteroidOS.
- **History:**
  - meta-smartwatch PR #227 (2023) added `nfcd-binder-plugin` for rubyfish but never started the HAL. Its status said "NFC is untested".
  - PR #283 (Feb 2026) removed it: "Removed work that doesn't work or hasn't been tested (like audio, nfc)".
  - Issue #175 never mentions NFC.

Raw evidence (surveys, decompiled live device tree, stock files, fetched sources) and a running log are in `~/.cache/sonic-screwdriver/research-nfc/` (`NOTES.md`).

## Hardware

| Item | Value | Source |
|---|---|---|
| NFC controller | ST21NFC core. Speaks NCI 2.0 as plain NCI over I2C. `compatible = "st,st21nfc"` | live DT, driver, stock HAL |
| Package | ST54-family NFC + eSE combo. Exact part (ST54H/J/K) unknown until its CORE_RESET_NTF is logged | `st,st54spi` node, Thales "GTO" eSE HAL in stock vendor |
| I2C | bus 6 (`i2c@7af6000`), address `0x08`, 400 kHz | live DT |
| IRQ | TLMM GPIO 21. The HAL sets the edge with an ioctl | `st,irq_gpio`, driver |
| Reset (no separate enable pin) | TLMM GPIO 20, active low. The driver drives it high (chip running) from probe | `st,reset_gpio`, driver |
| Firmware-download pin | none; ST chips enter their loader through a reset/IRQ sequence | driver `RECOVERY` ioctl |
| Clock request | PM660 GPIO 4 (global 1021) gates `clk_bb_clk2_pin`, a pin-controlled 19.2 MHz clock. It only runs while the chip asserts CLK_REQ, so the probe-time enable costs nothing at idle | DT, `msm-clocks-8952.h:311`, debugfs `bb_clk2_pin enable=1`, `bb_clk2 enable=0` |
| eSE | SPI bus 1 (`spi@78b5000`), CS 0, 10 MHz. SE power request on TLMM GPIO 71. Node `/dev/gto` | DT, `st54spi` driver |
| Ignore | `nq@28` on `i2c@7af5000` (bus disabled). Leftover from Qualcomm's NXP reference design | DT |

**How stock Wear OS used it** (from `~/Documents/sonic-screwdriver/stock-backup/vendor.img` and `system.img`):

- **HAL:** `/vendor/bin/hw/android.hardware.nfc@1.1-service.st` plus `/vendor/lib/nfc_nci.st21nfc.default.so`, reporting itself as "ST21NFC HAL1.1 Version 3.1.12".
  - It ran as `user nfc`.
  - `ueventd.rc` sets `/dev/st21nfc 0660 nfc nfc`.
  - AOSP has the source of a slightly newer version: `hardware/st/nfc`, 3.1.16.
- **Configs:**
  - `/vendor/etc/libnfc-hal-st.conf` and `libnfc-nci.conf`.
  - `/vendor/etc/st21nfc_conf.txt`: ST's RF, antenna and clock tuning. It's written into the chip's own memory and tagged CRC 6243, so it already lives in the chip.
- **No NFC firmware file was shipped.** `STNFC_FW_PATH_STORAGE=/vendor/firmware` holds none, and HAL 3.1.12 has no download code. The chip runs the firmware in its own flash.
- **eSE:** Thales/Gemalto `android.hardware.secure_element@1.0-service-gto` (`GTO_DEV=/dev/gto`), plus `SecureElement_st.apk` (OMAPI).
- **System:**
  - ST's fork of the Android NFC stack: `Nfc_st.apk`, `libstnfc-nci.so`;
  - `NfcPlacement.apk`, the "where is the antenna" app;
  - Google Pay (`PrebuiltTapAndPayWearable.apk`) with GMS.
- **So stock used NFC for Google Pay.** On Wear OS that's host card emulation (HCE) with tokenised cards. What the eSE was used for is unclear, perhaps regional wallets.

## Routes compared

| Route | What it would take | Effort | Risk | Verdict |
|---|---|---|---|---|
| **(a)** Kernel NFC subsystem + neard | No driver exists for this chip, even in mainline. `st-nci` only matches ST21NFCB/C and uses ST's older NDLC framing, which this chip doesn't speak. The 4.9 NCI core is NCI 1.x only (no CORE_RESET_NTF, no v2 CORE_INIT). It would need an NCI 2.0 backport, a new ~300-line driver, handling for the chip's self-reset after its proprietary mode-set (which the kernel's open sequence doesn't expect), a DT swap and dropping the vendor driver. neard itself is alive (v0.20, commits in Aug 2026) | weeks | kernel; high | **No.** nfcd even offers a neard-compatible D-Bus API (`dbus_neard` plugin) if a neard client is ever wanted |
| **(b)** Plain userspace stack on the char device | NXP's `linux_libnfc-nci` is for PN7120/PN7150 (PN7160 on another branch); its HAL does NXP-specific init and firmware download, so it can't drive an ST chip. ST publishes no Linux stack. Closest is [miromraz/stnfc](https://github.com/miromraz/stnfc), a raw NCI 2.0 CLI for the ST54J in a Pixel 4a. Its README reports these verified: tag polling, MIFARE Classic, APDUs, Type 4 NDEF emulation, HCE. It uses `/dev/i2c` + GPIO directly, which here belong to the kernel driver, so it would need porting to `/dev/st21nfc`. No D-Bus integration | days | low | **Fallback / debug tool** if (c) fails at the HAL level |
| **(c)** Android NFC HAL over hwbinder + nfcd + `nfcd-binder-plugin` | Everything is already on the watch except one plugin (+2 small libraries) and starting the HAL. Mobvoi's HAL already handles the chip's quirks (mode-set + self-reset, HCI credit, recovery) and was shipped on this exact chip and kernel. libhybris isn't involved: the HAL is its own Android process under hal-droid init, and nfcd talks to it over hwbinder through libgbinder, the way `bluebinder` talks to the Bluetooth HAL | hours | low | **Recommended** |

Notes on (c), checked against the ST HAL source:

- **The ST HAL hides the chip's quirks from the stack.** It swallows the first CORE_INIT_RSP, sends ST's mode-set (`2F 02 02 02 01`), hides the chip's self-reset (reason `0xA0`), re-initialises, and only then forwards CORE_INIT_RSP. To `libncicore` that just looks like a slow CORE_INIT.
- **The kernel driver accepts the HAL's ioctls.** The HAL uses the legacy `_IOR` ioctl codes (`PULSE_RESET`, `SET_POLARITY_*`, `GET_WAKEUP`), and Mobvoi's driver accepts both legacy and new codes. It also has `compat_ioctl` for the 32-bit userspace.
- **`coreInitialized` is skipped, probably harmlessly.**
  - `nfcd-binder-plugin` sends `coreInitialized` without its payload, so the HIDL stub rejects it; the plugin logs "CORE_INITIALIZED failed (that's ok)".
  - In the AOSP HAL that call only sends optional config, and the stock config has no `CORE_CONF_PROP` to send.
  - The stock 3.1.12 build appears to do its own config during open.
  - Probably harmless; listed under open questions.
- **nfcd copes with the HAL starting late or restarting.** The binder plugin watches hwservicemanager for `android.hardware.nfc@1.0::INfc/*`.

## Recommended route

### 1. Kernel: nothing to do

- **Effort 0, risk 0.**
- **Not needed in the patched kernel.** NFC needs nothing in the kernel planned in [brightness_fix.md](brightness_fix.md) and [touch_driver_fix.md](touch_driver_fix.md) (branches `brightness-fix` and `touch-driver-fix`). The NFC work can be done before or after it.
- **When that kernel is test-booted with `fastboot boot`**, add the NFC smoke test from [How to test](#how-to-test) to its checklist as a regression check. The driver is the same, so it should behave the same.
- **Only revisit if testing shows a problem.** Both fixes would be small and would ride along in that same kernel:
  - **Spurious wakeups from the NFC IRQ.** The driver arms it as a wake source and never frees it when the HAL closes.
  - **Idle drain from the eSE.** `st54spi` raises the eSE power request (GPIO 71) at probe, and only a HAL lowers it again. This 4.9 kernel can't show the line level without claiming the GPIO.

### 2. Firmware: nothing to do

- **Effort 0, risk 0.**
- **The chip already has its firmware.** It's in the chip's own flash, and no update image exists; stock shipped none.
- **Don't attempt a firmware update.** A failed ST update leaves the chip in loader mode.
- **Don't reapply the RF tuning.** `st21nfc_conf.txt` was already written into the chip by Wear OS (CRC 6243). Keep it as reference only.

### 3. Userspace

| Step | Effort | Risk |
|---|---|---|
| 3.1 Smoke test with prebuilt packages | 30–60 min | low; fully reversible |
| 3.2 If CORE_INIT fails: rebuild `libncicore` with two upstream fixes | half a day | low |
| 3.3 Optional: current nfcd stack, for card emulation | ~1 day | low–medium |
| 3.4 Make it permanent; send upstream | 1–2 h, plus review | low |
| 3.5 Something to use it with (AsteroidOS has no NFC app) | 1–2 days | low |

#### 3.1 Smoke test with prebuilt packages

1. **Install the plugin.**
   - The watch's opkg already points at `release.asteroidos.org/2.0/ipk/...`; 2.1 has no feed of its own.
   - Its glibc, glib, libgbinder, libglibutil and nfcd are exactly the versions those packages were built against.
   ```
   ssh sonic-screwdriver 'opkg update && opkg install nfcd-binder-plugin'
   ```
   - This pulls in `libncicore` 1.1.19 and `libnciplugin1` 1.1.4, about 47 KB in total.
   - `libncicore` installs files oddly named `libncicore.so.`; its SONAME really is `libncicore.so.`, so this is consistent and loads.
   - Don't run `opkg upgrade`.
   - Undo: `opkg remove nfcd-binder-plugin libnciplugin1 libncicore`.
2. **Start the HAL.** hal-droid init reads its rc files only at startup, so this needs a reboot. Restarting `android-init` would also restart the display composer.
   ```
   cat > /usr/libexec/hal-droid/system/etc/init/android.hardware.nfc@1.1-service.st.rc <<'EOF'
   service nfc_hal_service /vendor/bin/hw/android.hardware.nfc@1.1-service.st
       class hal
   EOF
   reboot
   ```
   - It runs as root, like on hoki and beluga: there's no `nfc` user here, and `/dev/st21nfc` is root-only.
   - Check it registered:
     ```
     ps | grep nfc@1.1
     /system/bin/lshal --neat -i | grep -i nfc    # expect android.hardware.nfc@1.1::INfc/default
     ```
   - Undo: delete the rc file and reboot.
3. **Run nfcd with debug logging,** without enabling it yet:
   ```
   mkdir -p /var/lib/environment/nfcd
   echo 'NFCD_ARGS=-v' > /var/lib/environment/nfcd/debug.conf   # -vv for even more
   systemctl start nfcd; journalctl -u nfcd -f
   ```
   - Expect the binder plugin to report `NFC adapter "default"`, then NCI reset and init, then RF discovery while the display is on.
   - **Failure signature:** `CORE_INIT (v1) failed (or is incomprehensible)`. In 1.1.19 the NCI 2.0 path prints "v1" by mistake. If you see it, go to 3.2.
   - Also note the CORE_RESET_NTF / manufacturer bytes in the log; they identify the exact chip and firmware.
4. **Tap a tag or card with the screen on,** then query nfcd (the object paths come from these calls):
   ```
   gdbus call --system --dest org.sailfishos.nfc.daemon --object-path / --method org.sailfishos.nfc.Daemon.GetAll3
   gdbus call --system --dest org.sailfishos.nfc.daemon --object-path /nfc0 --method org.sailfishos.nfc.Adapter.GetAll2
   dbus-monitor --system "sender='org.sailfishos.nfc.daemon'"
   ```
   - For a tag, use `org.sailfishos.nfc.Tag.GetAll3` and `GetNdefRecords`, then `org.sailfishos.nfc.NDEF.GetAll` on each record.
5. **If the HAL itself misbehaves,** read its logs with `/usr/libexec/hal-droid/system/bin/logcat | grep -i nfc` (tags `NfcHal`, `NfcNciHalWrapper`). For more detail:
   - copy `/vendor/etc/libnfc-hal-st.conf` to `/tmp`;
   - set `STNFC_HAL_LOGLEVEL=4` and `NFC_DEBUG_ENABLED=1`;
   - `mount --bind` the copy over the original, then restart the HAL.
6. **If discovery misbehaves,** limit the technologies the way hoki does: `/etc/libncicore.conf` with `[Configuration]` / `Technologies = A,B`.

#### 3.2 If CORE_INIT fails: rebuild libncicore

- **The bug:** before 1.1.33, `libncicore` requires the NCI 2.0 CORE_INIT_RSP to be exactly `2*n + 14` bytes. Any RF-interface extension stalls the state machine.
- **The fix:** backport two upstream commits onto 1.1.19:
  - `59ac8ba` (1.1.21): parse variable-length interface entries;
  - `e8fdbb9` (1.1.33): accept extensions. It was written for an ST54L, the Volla Phone Plinius.
- **Keep the SONAME `libncicore.so.`** so the feed's `libnciplugin`/binder plugin keep loading.
- **Build target:** armv7 hard-float against glibc ≤ 2.41, glib ≤ 2.84 and libglibutil 1.0.62.
- **Build environment:**
  - A Debian bookworm armhf cross toolchain in Docker works.
  - Apple Silicon can't run 32-bit ARM natively, so cross-compile rather than emulate.
  - Build libglibutil 1.0.62 first, for headers.
- **The nightly feed isn't an option:** it's built for glibc 2.42, newer than 2.1's.

#### 3.3 Optional: current nfcd stack (needed for card emulation)

- **Minimum versions:**
  - Card emulation needs **nfcd ≥ 1.2.0** and **libnciplugin ≥ 1.2.0** (both 2024).
  - ST-specific fixes need **libncicore ≥ 1.1.32**, which drops ActiveListenA because it "seems to confuse the hell out of st21nfc chip", and **≥ 1.1.33**.
  - Current versions: nfcd 1.2.8, libncicore 1.2.0, libnciplugin 1.2.3, nfcd-binder-plugin 1.2.2, plus libnfcdef.
  - The installed `mce.so` keeps working: nfcd only rejects plugins built for a *newer* nfcd. Copy it into the new plugin directory.
  - Build nfcd with `HAVE_DBUSACCESS=0`, as meta-asteroid master does.
- **Library clash:** these need **libglibutil ≥ 1.0.79**, but the watch has 1.0.62, which bluebinder, sensorfwd, mce and libgbinder all share.
- **Don't upgrade it system-wide.** Instead, install the new set under `/opt/nfc` and point only nfcd at it, with an `nfcd.service` drop-in:
  - `Environment=LD_LIBRARY_PATH=/opt/nfc/lib`;
  - an `ExecStart` override that runs `/opt/nfc/sbin/nfcd -p /opt/nfc/lib/nfcd/plugins`.
- Roll back by deleting the drop-in.

#### 3.4 Make it permanent and send upstream

- **On the watch:**
  - Keep the rc file.
  - Run `systemctl enable nfcd` and delete `debug.conf`.
  - Defaults can go in `/etc/nfcd/defaults.conf`:
    - `[Settings] Enabled=true`;
    - `[mce] RequireUnlock=false|true`.
  - The mce plugin powers NFC only while the display isn't off.
  - In this nfcd build, toggling NFC over D-Bus requires the group `privileged`, which doesn't exist. Use the settings file instead: `/var/lib/nfcd/settings`, `[Settings] Enabled=false`.
- **Upstream to AsteroidOS:**
  - **meta-smartwatch (meta-rubyfish):**
    - re-add `recipes-nemomobile/nfcd/nfcd_git.bbappend` with `RDEPENDS:${PN}:append:rubyfish = " nfcd-binder-plugin "`;
    - add the `nfc_hal_service` stanza to `recipes-android/android-init/android-init/init.rc`.
  - **meta-asteroid:** bump `libncicore` (≥ 1.1.33), `libnciplugin` and `nfcd`. That would help hoki and beluga too.
  - Then update the watch page.

#### 3.5 Something to use it with

AsteroidOS has no NFC app or client library; there's nothing in the AsteroidOS GitHub org. Options:

- **Tag-action daemon,** in Go like `ioslink`, on nfcd's D-Bus API. Tap a sticker, read its NDEF or UID, then:
  - show a notification; and/or
  - call a webhook over Wi-Fi, for example Home Assistant ("tap the sticker by the door").
- **Writing NTAG stickers:** `org.sailfishos.nfc.TagType2.WriteData`, or nfcd's `ndef-write` tool. It isn't packaged, so build it from `nfcd/tools`.
- **Sharing by tap (needs 3.3):** nfcd's `ndef-share` emulates a Type 4 NDEF tag holding a URL, contact or Wi-Fi config. iPhones read NDEF tags, but reading from an *emulated* tag is untested.

## What's realistic

| Feature | Realistic? | Notes |
|---|---|---|
| Detect tags and cards, read UID | yes | step 3.1 |
| Read NDEF: Type 2 (NTAG, Ultralight), Type 4 (e.g. DESFire with NDEF) | yes | the only tag types nfcd implements |
| Write NDEF to Type 2 tags | yes | D-Bus `TagType2`, `ndef-write` |
| Raw ISO-DEP APDUs (transit or ID cards with public data) | yes | `IsoDep.Transmit`; content depends on the card |
| Type 3 (FeliCa), Type 5 (ISO 15693), MIFARE Classic | detected only | nfcd has no drivers for these; it shows a generic tag with UID and raw `Transceive`. The chip itself can do more (stnfc reads MIFARE Classic) |
| Card emulation: NDEF tag, simple HCE app | probably, with 3.3 | proven on an ST54J by stnfc; untested here |
| Peer-to-peer (Android Beam/SNEP) | technically | pointless: Android dropped Beam, and iPhones never had it |
| Contactless payments | **no** | see below |
| Cloning badges or transit cards | **no** | see below |

### Payments: not feasible

- **Phone-style payments (HCE).** Google Wallet, Apple Pay, Garmin Pay and the like use card tokens that Visa and Mastercard issue only to certified wallet apps on attested devices. None of those apps can run on AsteroidOS, and nothing here can pass Google's device attestation. This is a policy wall, not a software gap.
- **The secure element.** The eSE is locked. Installing a payment applet needs the eSE's issuer-domain keys, held by Mobvoi, Thales or ST, and then personalisation by the bank. The stnfc author reached the same conclusion for the ST54J in the Pixel 4a ("dead silicon").
- **Copying a physical card.** Each tap is signed with keys that never leave the card's chip, so a bank card can't be copied. The same goes for most transit and office cards.
- **MIFARE Classic badges.** ST controllers can't emulate MIFARE Classic at all, so those badges can't be cloned either.

## How to test

1. **Baseline:**
   - the driver probe lines in `journalctl -k -b 0`;
   - `/dev/st21nfc` and `/dev/gto` exist;
   - zero `pm_system_irq_wakeup ... st21nfc` lines.
2. **HAL:**
   - `ps` shows `android.hardware.nfc@1.1-service.st`;
   - `lshal` lists `INfc/default`;
   - no crash loop in `logcat`.
3. **nfcd:** the journal shows the adapter added and NCI init OK. `Adapter.GetAll2` shows:
   - `Enabled` true;
   - `Powered` true with the screen on, and false with it off.
4. **Tags:**
   - an NTAG sticker gives a `TagsChanged` signal, the right type and its NDEF records;
   - then try a bank card (ISO-DEP, no NDEF) and any transit or ID cards.
   - Also find the antenna sweet spot (the stock `NfcPlacement` app showed it) and note it here.
5. **Battery:**
   - check whether always-on (LPM) counts as "off" for nfcd (`Powered` false);
   - over one night with nfcd enabled and one without, compare the battery drop;
   - grep the kernel journal for NFC wakeups.
6. **Robustness:**
   - 50 taps in a row;
   - suspend and resume with the HAL open;
   - `systemctl restart nfcd`;
   - kill the HAL; init restarts it, and nfcd should pick it up again.
7. **Patched kernel:** rerun 1–4 on the `fastboot boot` test of the brightness/touch kernel.

## Open questions

1. **Which exact chip and firmware version is it** (ST54H/J/K)? The first `nfcd -v` run logs the CORE_RESET_NTF that tells.
2. **Does its CORE_INIT_RSP list RF-interface extensions?** This decides between 3.1 alone and 3.2.
3. **Does skipping the HAL's `coreInitialized` matter for ST HAL 3.1.12?** It's probably harmless (see [Routes compared](#routes-compared)). If not, the fix belongs in `nfcd-binder-plugin`: send the CORE_INIT_RSP bytes as the payload.
4. **Where is the antenna, and how good is the read range?**
5. **Does mce report the always-on (LPM) display as "off"?** If so, NFC is only live with the screen fully on, which is good for the battery. `[mce] AlwaysOn` would change that.
6. **eSE idle power:** is GPIO 71 held high, keeping the secure element powered, and does it matter?
   - Measure it.
   - A HAL session may lower it on close, through the driver's "eSE not used" callback.
   - If not, a small `st54spi` change could go in the patched kernel.
7. **Can an iPhone read an NDEF tag emulated by the watch** (step 3.5)?
