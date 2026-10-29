# GPS (to do)

Get position fixes from the watch's built-in GPS on AsteroidOS, and make them available to apps. GPS has never produced a fix on this port. asteroidos.org rates it "partial", but nothing in the 2.1 image backs that up (see [What "Partial" means today](#what-partial-means-today)).

Status (2026-10-29): researched, nothing changed on the watch.

Paths under `stock-backup/` are in `~/Documents/sonic-screwdriver/`.

- **The hardware works, and no kernel change is needed.**
  - The GPS is a dedicated **Broadcom BCM4775x** chip on SPI. It is not the Snapdragon's modem GNSS, so the offline modem doesn't matter.
  - Its driver is built into the running kernel and already probes at every boot. The device nodes exist, and the chip is powered.
- **No firmware is missing.** The chip's RAM patch is on the watch's vendor partition, and Broadcom's own daemon loads it at every start. Nothing gets flashed.
- **What's missing is all userspace.**
  - Stock Wear OS ran three Broadcom processes from `/vendor`: `lhd`, `gpsd` and the GNSS HAL. AsteroidOS starts none of them. Its only GNSS init entry points at a Qualcomm binary that this watch doesn't have.
  - No location stack is installed: no geoclue, no provider, no Qt Positioning.
- **The recommended route is the same one the OPPO Watch uses** (GPS rated "good" there): start the vendor HAL from hal-droid init, then add geoclue and `geoclue-provider-hybris-binder` from the 2.0 feed. That's 3 packages, about 240 KiB.
- **The first test needs no install and no reboot.** Run `lhd` and `gpsd` by hand, and read NMEA from a FIFO, outdoors.
- **The iPhone can't share its location over standard BLE.** Only a custom iOS app could. Assisted GPS is better served by Broadcom's LTO orbit files, and those servers are still up.

Raw evidence (device tree, driver sources, stock vendor files and string dumps, cloned sources, watch surveys) and a running log are in `~/.cache/sonic-screwdriver/research-gps/`. Start from `NOTES.md` there.

## Current state (evidence, 2026-10-29)

### Kernel: ready

- **Device tree** (`/sys/firmware/devicetree/base/soc/spi@78b7000/gps`, the same as Mobvoi's `rover-gps.dtsi`):
  - `compatible = "ssp,bcm4775"` on SPI bus 3, CS0, 16 MHz, SPI mode 3;
  - interrupt and `ssp-host-req` on TLMM 34; `ssp-mcu-req` on 41; `ssp-mcu-resp` on 40;
  - `ssp-nstandby` on 42; `power_enable` on 43.
- **Config**, identical in stock and AsteroidOS: `CONFIG_SENSORS_SSP_BBD=y`, `CONFIG_BCM_GPS_SPI_DRIVER=y`. That's Broadcom's "BBD" (Broadcom Bridge Driver), built in, so no module is involved.
  - Source: `drivers/char/brcm/bbdpl/{bbd.c,bcm_gps_spi.c,…}` at `c428ef36`.
- **The driver is bound:** `spi3.0` uses driver `brcm gps spi`.
- **Device nodes:** `/dev/ttyBCM` (misc 10,59), `/dev/bbd_control`, `/dev/bbd_sensor` and `/dev/bbd_patch` (major 229), plus `/sys/bbd/`. All are `root:root 0600`; stock gave them to the `gps` user.
- **The chip is powered and out of standby since boot.**
  - `/sys/class/gpio/gpio43/value` (power load switch) = 1, and `gpio42/value` (nstandby) = 1. The probe function does this.
  - TLMM's GPIO base is 0, so stock's paths `/sys/class/gpio/gpio4{2,3}/value` are valid here.
- **Nothing has ever opened it:** `/proc/interrupts` shows `268 msmgpio 34 Level ttyBCM` at **0** after 1.5 h of uptime.
- **No kernel log lines.** The driver logs only with `pr_debug`, and `CONFIG_DYNAMIC_DEBUG` is off.
- **No remote processor involved.**
  - The chip has its own RF front end, a 26 MHz TCXO (`FRQ_PLAN_26MHZ_2PPM`) and an external LNA.
  - The `memshare_GPS` line at boot belongs to Qualcomm's modem GNSS, which nothing uses.

### Userspace: nothing

- **hal-droid init has a broken GNSS entry.** `/usr/libexec/hal-droid/system/etc/init/android.hardware.gnss@1.0-service.rc` fails at every boot (this boot and last):
  ```
  init: Could not start service 'vendor.gnss_service' as part of class 'hal': Cannot find '/vendor/bin/hw/android.hardware.gnss@1.0-service'
  ```
  - That rc comes with AsteroidOS's generic hal-droid tarball, `hybris-p-msm8909w.tar.gz`, which was built for Snapdragon Wear 2100/3100 watches with Qualcomm GNSS.
  - Rover's vendor partition has Broadcom's `android.hardware.gnss@1.1-service` instead.
- **No GNSS HAL is registered:** `lshal` lists no `gnss`. No `lhd` or `gpsd` process runs, and `/data/vendor/gps` doesn't exist.
- **No location packages.** Of 714 installed packages, there is no geoclue, provider, Qt Location or Positioning, or gpsd.
  - `/usr/bin/test_gps` (from libhybris) is installed.
- **No location D-Bus service** on the system or session bus, active or activatable.
- **What's already in place:**
  - `ro.hardware.gps=brcm` is already set. It's loaded from the stock `/system/etc/prop.default`, and it is the trigger stock used to start `lhd` and `gpsd`.
  - `/var/lib/location/location.conf` (from `libsystemsettings1`) already says `enabled=true`, `gps\enabled=true`.
  - `ceres` is in group `gps` (997, created by `asteroid-users`).
  - `/dev/hwbinder` is `0777`.
- **The vendor binaries match stock and will link.**
  - `/vendor/bin/gpsd`, `/vendor/bin/lhd` and `/vendor/etc/SensorHub.patch` have the same md5 as the stock backup.
  - Every library they and the HAL need (`libssl`, `libxml2`, `libhardware_legacy`, `android.frameworks.sensorservice@1.0`, `android.hardware.gnss@1.0/1.1`, …) is in `/system/lib` or `vndk-28`, and `ld.config.28.txt` links it.

## Hardware

| Item | Value | Source |
|---|---|---|
| GNSS chip | **Broadcom BCM4775x** (dual-frequency-capable GNSS with an on-chip "hub" MCU), silicon **rev B1**. The engine supports 47751–47758; the exact part number shows in `gpsd`'s `chip.info` on first run | patch header `AsicVersion=0x004775B1`, `gpsd` chip table |
| Bus | SPI bus 3 (`spi@78b7000`, Linux `spi3.0`), CS0, 16 MHz, mode 3, Broadcom "SSI" stream protocol | DT, `bcm_gps_spi.c` |
| Handshake | host-req TLMM 34 (input; IRQ, level high), mcu-req 41 (output), mcu-resp 40 (input, pull-up). "GPIO 40 reused by the GPS chip" is this mcu-resp line | DT, `rover-gps.dtsi` |
| Power / standby | TLMM 43 drives the chip's power load switch ("vdd_pmu_in"), TLMM 42 is nstandby. The driver sets both high at probe and exports both to sysfs, plus an `nstandby` attribute on `spi3.0` | `bcm_gps_spi.c` probe |
| RF | 26 MHz TCXO, external LNA. Mobvoi's config runs **L1 only** (`MultiCarrRFMode=GL_MULTI_CARR_RF_MODE_L1`), although the LNA mask enables an L5 path and a factory "L5 CW" test job exists | stock `gps.xml` |
| Constellations | GPS, GLONASS, BeiDou, Galileo, QZSS (Mobvoi spec). The engine also knows SBAS and NavIC | mobvoi.com, `gpsd` strings |
| Firmware | `/vendor/etc/SensorHub.patch` (364 KB, "\Hub" application patch, ChangeList 454179). It's a RAM patch downloaded by `lhd` at each start; the chip has no flash to write | patch header, `lhd.conf` |
| Model coverage | **rubyfish (Pro 3 GPS) and rover (Pro 3 Ultra / LTE) have the same chip.** Both `rubyfish-overlay.dts` and `rover-overlay.dts` include `rover-gps.dtsi`. rubyfish is SDA429W, which has no modem at all | Mobvoi kernel tree |
| Not involved | The Snapdragon modem GNSS (no Qualcomm location daemons in stock vendor). The **nanohub** MCU: its driver, DT and sensor HAL have no GNSS references | stock vendor, DT, `sensors.nanohub.so` |

### How stock Wear OS used it (from `stock-backup/vendor.img` and `system.img`)

```
Android LocationManager
  -> android.hardware.gnss@1.1-service        (HIDL; IGnss/default + IGnss/gnss_vendor; user system)
  -> android.hardware.gnss@1.1-impl-brcm.so
  -> /vendor/lib/hw/gps.default.so            (thin IPC client "GpsiClient"; also flp.default.so)
  -> /dev/socket/gps  (seqpacket, created by init for gpsd)
  -> /vendor/bin/gpsd -c /vendor/etc/gps.xml  (Broadcom GLL 116.20.23, 2020: the positioning engine, SUPL, LTO)
  -> pipe in /data/vendor/gps/
  -> /vendor/bin/lhd /vendor/etc/lhd.conf     (Broadcom "Location Hub Daemon" LHD2 116.20: patch download, power mgmt)
  -> /dev/ttyBCM + /dev/bbd_control + /dev/bbd_sensor -> BCM4775x
```

- **Startup:** `/vendor/etc/init/hw/init.gps.rc` creates `/data/vendor/gps` and gives the device nodes and GPIO 42/43 `value` files to `gps:system`. Then `on property:ro.hardware.gps=brcm`, it starts `lhd` and `gpsd`.
- **`gps.xml`, worth knowing:**
  - `NmeaOutName="/data/vendor/gps/gpspipe"`: an NMEA FIFO.
  - `CtrlPipe="/data/vendor/gps/glgpsctrl"`: a control pipe taking `$pglirm,…` commands.
  - `HttpSyncLto="true"`: `gpsd` downloads Broadcom's LTO orbit file itself.
  - `SuplEnable="true"`.
  - `SensorsEnable="true"`: sensor fusion through Android's SensorService.
  - `WakeLock="geo"`.
  - A job named `Periodic` (1,000 fixes at 1 Hz).
- **`gpsd` can also run standalone.** Its usage string is `[configFile] [jobName]`, and it logs "Starting GpsRun to run [%s] job from [%s] file". That allows a test without any HAL.

## What "Partial" means today

- **The rating has nothing behind it in the shipped image.** On 2.1's rubyfish image no HAL starts, no geoclue or provider is installed, and no app asks for location. Today no app can get a location, on rubyfish or rover.
- **Where the rating came from.** It was set when rubyfish was added to the site: [asteroidos.org commit e3eac4946a](https://github.com/AsteroidOS/asteroidos.org/commit/e3eac4946a), 2026-02-08, by MagneFire, `"GPS", "status":"partial"`, with no comment.
- **The work behind it was dropped.**
  - [PR #227](https://github.com/AsteroidOS/meta-smartwatch/pull/227) (argosphil, 2023) added `geoclue-provider-hybris-binder` for rubyfish and said "GPS is untested". It never started the Broadcom stack, so it couldn't have worked.
  - [PR #283](https://github.com/AsteroidOS/meta-smartwatch/pull/283), merged five days after the site entry, superseded #227 and "Removed work that doesn't work or hasn't been tested". Master's `meta-rubyfish` has no geoclue bbappend and no GNSS service.
- **Best guess:** "chip detected, driver probes, not integrated". It could also reflect a manual test I can't see. Worth asking MagneFire in [#175](https://github.com/AsteroidOS/meta-smartwatch/issues/175) or on Matrix.
- **For comparison,** among the 15 watches the site rates for GPS:
  - beluga (OPPO Watch) and pike are "good";
  - catfish and rubyfish are "partial";
  - the other 11 are "bad", hoki among them since 2024.

## Routes compared

| Route | What it takes | Effort | Risk | Verdict |
|---|---|---|---|---|
| **(a)** Vendor Broadcom stack under hal-droid init + geoclue + `geoclue-provider-hybris-binder` (HIDL over hwbinder) | 3 rc stanzas, 2 config copies, 3 packages from the 2.0 feed. The same pattern as beluga, whose dedicated Sony CXD5603 chip + vendor HIDL HAL is started from AsteroidOS's `init.rc` | hours | low | **Recommended** |
| **(b)** Vendor `lhd` + `gpsd` only, reading the NMEA FIFO and driving `gpsd` through jobs or its control pipe | No HAL, no binder, no geoclue. But the control-pipe syntax is undocumented, and an app integration would have to be written | hours (test), days (product) | low | **First smoke test** and debug tool |
| **(c)** Like (a), but `geoclue-provider-hybris-hal` (libhybris loads `gps.default.so` in-process) instead of the HIDL service | One process fewer. It's the pike/sawfish pattern, and `test_gps` exercises the same path | hours | low–medium | **Plan B** if the binder provider misbehaves |
| **(d)** Open-source driver or engine | Broadcom's chip protocol and patch format are proprietary. No free implementation of any BCM477x engine exists | months | high | **No** |
| **(e)** Snapdragon modem GNSS | Stock never used it; rubyfish has no modem; it would need the modem booted, Qualcomm's loc stack and an RF path that may not exist | weeks | high | **No** |

## Recommended route

### 1. Kernel: nothing to do

- **Effort 0, risk 0.** Today's kernel already has everything; GPS needs nothing in the shared kernel planned in [brightness_fix.md](brightness_fix.md#plan-build-test-flash), [touch_driver_fix.md](touch_driver_fix.md) and [sound.md](sound.md#kernel-one-build-with-the-brightness-and-touch-fixes) (branches `brightness-fix`, `touch-driver-fix` and `sound`). The driver is built in, so sound's tracing and module-signing changes don't affect it.
- **When that kernel is test-booted**, add this regression check to its checklist:
  ```
  ls -l /dev/ttyBCM /dev/bbd_control /dev/bbd_sensor /dev/bbd_patch
  readlink /sys/bus/spi/devices/spi3.0/driver          # .../brcm gps spi
  cat /sys/class/gpio/gpio42/value /sys/class/gpio/gpio43/value   # 1 1
  ```
  If the userspace below is set up by then, also do one outdoor fix (test T3).
- **Only if testing shows a need**, these could ride along in the same kernel:
  - `ccflags-y += -DDEBUG` in `drivers/char/brcm/bbdpl/Makefile`, to get the driver's SPI and handshake logs. It's noisy, so use it in test builds only.
  - **A cosmetic resume bug.** `bcm_spi_resume()` re-enables the IRQ even when nobody has `/dev/ttyBCM` open. It's harmless today: the IRQ count is 0 and the IRQ isn't wake-armed.
  - **Holding the chip in standby at probe.** Only if [T5](#how-to-test) shows the unmanaged chip costs real battery and a userspace fix isn't enough. Prefer userspace: a running `lhd` manages standby itself, or write `0` to `nstandby` at boot when GPS isn't wanted.

### 2. Firmware: nothing to do

- **Effort 0, risk 0.** `SensorHub.patch` is already on the watch's `/vendor`, identical to stock. `lhd` downloads it into the chip's RAM at every start, and it's gone at power-off. The modem partition's firmware is not involved.
- **Assistance data is data, not firmware.** It's covered under [Assisted GPS](#assisted-gps).

### 3. Userspace

| Step | Effort | Risk |
|---|---|---|
| 3.1 Smoke test by hand: `lhd` + `gpsd` + NMEA FIFO; no install, no reboot | 30 min + one walk outside | low; stop with `killall`, reboot for a clean chip state |
| 3.2 Define the three Broadcom services in hal-droid init; start and stop them by hand | 1 h, one reboot | low; services are `disabled` until you start them |
| 3.3 geoclue + binder provider from the 2.0 feed; a test client | 1–2 h | low; `opkg remove` undoes it |
| 3.4 Make it permanent; send upstream | 1–2 h + review | low |
| 3.5 Apps: Qt Positioning, `asteroid-gps-test`, later a fitness or map app | 1–3 days | low |

#### 3.1 Smoke test by hand (route b)

Proves the chip, patch download, engine and antenna before any integration work. Everything runs as root, like AsteroidOS's other vendor services.

```sh
ssh sonic-screwdriver
# AsteroidOS copies of the stock configs (/vendor is read-only)
mkdir -p /etc/gps /data/vendor/gps/log/lhd /data/vendor/gps/log/gps
cp /vendor/etc/lhd.conf /vendor/etc/gps.xml /etc/gps/
# gpsd would ask Android's SensorService for motion data; AsteroidOS has none,
# and Android 9's getService() can wait forever for it
sed -i 's/SensorsEnable="true"/SensorsEnable="false"/' /etc/gps/gps.xml
# Broadcom's own log files, while testing
sed -i 's/LogEnabled="false"/LogEnabled="true"/' /etc/gps/gps.xml
sed -i 's/^LogEnabled=false/LogEnabled=true/' /etc/gps/lhd.conf

# 1. Location Hub Daemon: opens /dev/ttyBCM and downloads SensorHub.patch
/vendor/bin/lhd /etc/gps/lhd.conf > /tmp/lhd.out 2>&1 &
sleep 5
grep ttyBCM /proc/interrupts            # the count must now be above 0
/usr/libexec/hal-droid/system/bin/logcat -d | grep -i -E 'lhd|esw|patch|asic' | tail -40

# 2. GPS engine running the "Periodic" job from gps.xml (no HAL client needed)
/vendor/bin/gpsd -c /etc/gps/gps.xml Periodic > /tmp/gpsd.out 2>&1 &
sleep 5
/usr/libexec/hal-droid/system/bin/logcat -d | grep -i -E 'gpsd|GpsRun|job|glgps' | tail -20
ls -l /data/vendor/gps/                 # expect gpspipe (FIFO), glgpsctrl, chip.info, NV files
cat /data/vendor/gps/chip.info          # exact chip, e.g. "Broadcom,BCM4775x,..."
cat /data/vendor/gps/gpspipe            # live NMEA; Ctrl-C stops reading only

# stop
killall gpsd lhd
```

- **If `gpsd` doesn't start the job,** try the positional form from its usage string: `gpsd /etc/gps/gps.xml Periodic`. Logcat should say `Starting GpsRun to run [Periodic] job from [...] file`.
- **What success looks like indoors:**
  - `ESW:READY` from `lhd`;
  - `GSV` sentences listing satellites, perhaps with low signal;
  - `GGA` with fix quality 0.
- **Outdoors:** a `GGA` with fix quality ≥ 1. The very first start has no LTO or saved data, so it can take several minutes; later starts should be much quicker. Run it with [T2](#how-to-test), since SSH doesn't reach the watch outside.
- **After stopping, reboot** if you want the chip back in its untouched boot state. After `lhd` exits, the chip keeps running its patch.

#### 3.2 The Broadcom services in hal-droid init

On stock, init creates `/dev/socket/gps` for `gpsd` (the rc's `socket` option), and the HAL connects to it. So run all three under hal-droid init, the way AsteroidOS runs its other vendor services. hal-droid init reads rc files only at startup, so this needs one reboot. After that, start and stop the services by hand with `/system/bin/start` and `/system/bin/stop` (or `setprop ctl.start …`).

```sh
cat > /usr/libexec/hal-droid/system/etc/init/gnss-brcm.rc <<'EOF'
# Broadcom BCM4775x GNSS (rubyfish/rover). Mirrors stock /vendor/etc/init/hw/init.gps.rc and
# android.hardware.gnss@1.1-service.rc, but runs as root like AsteroidOS's other HALs and uses
# the config copies in /etc/gps. All services are disabled: nothing starts by itself yet.
on init
    mkdir /data/vendor/gps 0770 root root
    mkdir /data/vendor/gps/log 0770 root root

service vendor.gps.lhd /vendor/bin/lhd /etc/gps/lhd.conf
    class gps
    disabled

service vendor.gps.gpsd /vendor/bin/gpsd -c /etc/gps/gps.xml
    class gps
    disabled
    socket gps seqpacket 0660 root root

service vendor.gnss-1-1 /vendor/bin/hw/android.hardware.gnss@1.1-service
    class gps
    disabled
EOF
reboot
```

After the reboot:

```sh
/system/bin/start vendor.gps.lhd; sleep 3
/system/bin/start vendor.gps.gpsd; sleep 2
ps | grep -E 'lhd|gpsd' | grep -v grep
# optional middle-layer test, BEFORE starting the HIDL service:
# libhybris loads gps.default.so, which talks to gpsd
test_gps -t                               # prints NMEA and fixes; Ctrl-C to end
/system/bin/start vendor.gnss-1-1
/system/bin/lshal --neat -i | grep -i gnss  # expect android.hardware.gnss@1.0::IGnss/default AND @1.1::IGnss/default
```

- **Stop:** `/system/bin/stop vendor.gnss-1-1; /system/bin/stop vendor.gps.gpsd; /system/bin/stop vendor.gps.lhd`.
- **Undo:** delete the rc file and reboot.
- **Leave the broken `android.hardware.gnss@1.0-service.rc` alone.** It's owned by the hal-droid package and is harmless; use different service names, as above.

#### 3.3 geoclue and the binder provider

- **Install** (the 2.0 feed already in `/etc/opkg`; it matches 2.1's libraries):
  ```sh
  opkg update
  opkg install geoclue geoclue-provider-hybris-binder   # pulls libqofonoext1; 3 packages, ~240 KiB
  su ceres -c "XDG_RUNTIME_DIR=/run/user/1000 systemctl --user daemon-reload"
  ls -l /usr/libexec/geoclue-hybris    # setuid root, as the recipe installs it
  ```
- **What's installed:**
  - `geoclue` 0.12.99 (the Geoclue **1** API that Sailfish and AsteroidOS use).
  - The provider, `+git0+747fc6ecd1` = tag 0.2.35. It's a session-bus service, `org.freedesktop.Geoclue.Providers.Hybris`, started on demand through a systemd user unit.
- **How the provider reaches the GPS:** it talks to `android.hardware.gnss@1.0::IGnss/default` over hwbinder. Broadcom's `@1.1` HAL also serves the `@1.0` interface, and its `setCallback`, `injectTime`, `injectLocation`, XTRA and AGNSS paths are all there (checked against the implementation's symbols).
- **No settings change is needed.** The provider turns GPS on only if LocationSettings say so. `/var/lib/location/location.conf` (the file it reads) already has `enabled=true` and `gps\enabled=true`. `/etc/location/location.conf` with `enabled=false` is only a compatibility copy.
- **Test with debug logs.** In one SSH session, as `ceres`, run the provider in the foreground (Sailfish's recipe):
  ```sh
  su ceres -c "XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus QT_LOGGING_RULES='geoclue.provider.hybris*.debug=true' /usr/libexec/geoclue-hybris"
  ```
  - In a second session, run a client that holds a reference ([test client](#test-client)).
  - Expect GNSS init lines, status changes and NMEA (category `geoclue.provider.hybris.nmea`), then `Set location` outdoors.
  - To watch the client side: `dbus-monitor --session "sender='org.freedesktop.Geoclue.Providers.Hybris'"`, as `ceres`.
- **Undo:** `opkg remove geoclue-provider-hybris-binder geoclue libqofonoext1`.
- **Plan B:** `opkg remove geoclue-provider-hybris-binder && opkg install geoclue-provider-hybris-hal`, and stop `vendor.gnss-1-1`. That provider loads `gps.default.so` in-process; both install `/usr/libexec/geoclue-hybris`, so it's one or the other.

#### 3.4 Make it permanent and send upstream

- **On the watch:**
  - add `start vendor.gps.lhd`, `start vendor.gps.gpsd` and `start vendor.gnss-1-1` at the end of the rc's `on init` block;
  - set `LogEnabled` back to `false` in both configs.
  - Idle cost: `lhd` and `gpsd` sit idle until a client asks for a fix. Check with [T5](#how-to-test) before leaving them running.
- **Upstream** to meta-smartwatch `meta-rubyfish` (it covers rover too):
  - the three services (and the mkdirs) in `recipes-android/android-init/android-init/init.rc`;
  - drop the generic `android.hardware.gnss@1.0-service.rc` for rubyfish;
  - re-add PR #227's one-liner `recipes-navigation/geoclue/geoclue_%.bbappend`: `RDEPENDS:${PN}:append:rubyfish = " geoclue-provider-hybris-binder "`;
  - the two config tweaks. Better generated at boot from `/vendor/etc` (copy + `sed`) than shipped, to avoid redistributing Mobvoi's files;
  - then correct the GPS rating on the site, with a comment, and post in #175.
- **No base image includes geoclue on any watch.** It arrives when an app depending on Qt Location is installed. The bbappend only makes sure the provider comes along.

#### 3.5 Apps

- **Qt Positioning:** `opkg install qtlocation qtlocation-plugins qtlocation-qmlplugins`, about 1 MiB. AsteroidOS's 2.x build deletes Qt's Geoclue-2 plugin, so positioning goes through the Geoclue-1 plugin and the provider above.
- **`asteroid-gps-test`:** a QML `PositionSource` (`SatellitePositioningMethods`, 1 s). It's in meta-asteroid's 2.0 branch but not in the feed, so it needs an SDK build.
- **Fitness and map apps:** see the next section.

## How apps would consume location

```
QML PositionSource / QGeoPositionInfoSource (Qt5 QtPositioning, "geoclue" v1 plugin)
   -> geoclue master (org.freedesktop.Geoclue.Master, session bus; picks a provider by accuracy, RequiresGPS)
   -> org.freedesktop.Geoclue.Providers.Hybris   <- native clients can talk to it directly (ioslink, Go, C)
   -> hwbinder: android.hardware.gnss@1.0::IGnss/default (Broadcom @1.1 HAL)
   -> gps.default.so -> /dev/socket/gps -> gpsd -> lhd -> /dev/ttyBCM -> chip
                                           gpsd also writes raw NMEA to /data/vendor/gps/gpspipe (root only)
```

1. **QML or Qt apps** (fitness, maps):
   - `import QtPositioning 5.x` with `PositionSource { preferredPositioningMethods: PositionSource.SatellitePositioningMethods; updateInterval: 1000; active: true }`.
   - Satellites in view and in use are available in C++ through `QGeoSatelliteInfoSource`.
   - Maps would add Qt Location's `Map` with the OSM plugin, which needs tiles over Wi-Fi or a local tile cache.
2. **Native daemons, ioslink for example,** can skip the master and call the provider on the **session bus**:
   - name `org.freedesktop.Geoclue.Providers.Hybris`, object `/org/freedesktop/Geoclue/Providers/Hybris`.
   - `org.freedesktop.Geoclue.AddReference()` keeps the GPS on **for as long as the calling D-Bus connection stays open**, so one-shot `dbus-send`, `gdbus call` or `busctl call` can't hold it. End with `RemoveReference()`, or just disconnect.
   - `SetOptions(a{sv})` with `{"UpdateInterval": uint32 ms}`; the minimum is 1,000 ms.
   - `.Position`: `GetPosition` and signal `PositionChanged(i fields, i timestamp, d lat, d lon, d alt, (idd) accuracy)`. Fields are lat `0x1`, lon `0x2`, alt `0x4`; accuracy is level, horizontal, vertical.
   - `.Velocity`: `VelocityChanged(i fields, i ts, d speed, d direction, d climb)`.
   - `.Satellite`: `SatelliteChanged(i ts, i used, i visible, ai used_prn, a(iiii) sat_info)`.
   - `GetStatus` / `StatusChanged`: 0 error, 1 unavailable, 2 acquiring, 3 available.
   - The provider exits 30 s after its last client and reports fix-lost after 30 s without a fix.
3. **Raw NMEA:** `cat /data/vendor/gps/gpspipe` while a session runs. Useful for logging, debugging, or feeding the Linux `gpsd(8)` (not packaged). It's root-only, so not for apps.
4. **Screen off.** `gpsd` holds an Android wakelock (`WakeLock="geo"`, and this kernel has `CONFIG_PM_WAKELOCKS`), so fixes should keep coming during a workout with the display off. That also means the watch stays awake for the whole session. To verify in T3/T4.
5. **Nightlies (Qt 6) use the same backend.** The provider is 0.2.40 without systemsettings, with `qtpositioning` + `qtpositioning-geoclue`. App code stays QtPositioning.

### Test client

About 40 lines of Go, built like ioslink (`GOOS=linux GOARCH=arm GOARM=7`). Run it as `ceres` with the session-bus environment:

```go
// geotest: hold a GPS session open on the geoclue hybris provider and print what it reports.
package main

import (
	"fmt"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/godbus/dbus/v5"
)

const svc = "org.freedesktop.Geoclue.Providers.Hybris"

func main() {
	conn, err := dbus.ConnectSessionBus()
	if err != nil {
		panic(err)
	}
	if err := conn.AddMatchSignal(dbus.WithMatchSender(svc)); err != nil {
		panic(err)
	}
	sigs := make(chan *dbus.Signal, 32)
	conn.Signal(sigs)

	obj := conn.Object(svc, "/org/freedesktop/Geoclue/Providers/Hybris")
	// The reference lives as long as this connection: that is what keeps the GPS on.
	if err := obj.Call("org.freedesktop.Geoclue.AddReference", 0).Err; err != nil {
		panic(err)
	}
	obj.Call("org.freedesktop.Geoclue.SetOptions", 0,
		map[string]dbus.Variant{"UpdateInterval": dbus.MakeVariant(uint32(1000))})

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	for {
		select {
		case s := <-sigs:
			fmt.Println(time.Now().Format("15:04:05"), s.Name, s.Body)
		case <-stop:
			obj.Call("org.freedesktop.Geoclue.RemoveReference", 0)
			return
		}
	}
}
```

## Assisted GPS

What speeds up a fix, and what's realistic here:

| Aid | Source | Status |
|---|---|---|
| Orbit data: **Broadcom LTO** (7-day "Long Term Orbits", Broadcom's equivalent of XTRA) | `http://gllto.glpals.com/7day/v5/latest/lto2.dat` (also `gllto1`/`gllto2`, and a 4-day variant) | **Servers alive:** HTTP 200, 181,695 bytes, regenerated on 2026-10-29. `gpsd` fetches it itself (`HttpSyncLto="true"`, refresh after 3 days) whenever it runs with network access |
| Time | System clock: CTS from the iPhone via ioslink, and NTP over Wi-Fi. The provider also injects NTP time from connman's time servers when online | already good |
| Last position, almanac, ephemeris | `gpsd`'s NV files in `/data/vendor/gps/`, which persist on the rootfs. The chip itself stays powered all the time on this watch | hot and warm starts should be quick; verify |
| Coarse position injection | **Any** session-bus `org.freedesktop.Geoclue.Position.PositionChanged` signal. The provider injects it with `IGnss::injectLocation` until its own first fix, so no provider change is needed | a hook for phone or Wi-Fi positions |
| SUPL (`supl.google.com:7275`) | Needs a data connection *during* the fix plus a cell or WLAN location ID. This watch has only Wi-Fi outdoors (the LTE modem is unused) | skip |
| Wi-Fi geolocation (BeaconDB, the community replacement for Mozilla Location Service) | Needs the watch online, which outdoors it isn't | low value; skip |

- **Will `gpsd`'s own LTO download work?** Unverified.
  - On Android, `gpsd` resolves names through bionic, which normally asks netd. With no netd, bionic falls back to its own resolver.
  - That resolver ignores `/etc/resolv.conf` and defaults to the loopback address, where connman's DNS proxy is listening (127.0.0.1:53 and [::1]:53, checked in `/proc/net`). So it may just work.
  - **Check:** after a session on Wi-Fi, see whether an LTO file appears in `/data/vendor/gps/`.
  - **Fallback:** a small systemd timer runs BusyBox `wget -O /data/vendor/gps/<file>.tmp http://gllto.glpals.com/7day/v5/latest/lto2.dat && mv …` every 2–3 days on Wi-Fi, using the filename `gpsd` uses. `gpsd` reads LTO from `LtoDir`.
- **The provider's own XTRA download path is off on AsteroidOS.** It only runs when an HERE or MLS provider exists. That's fine; `gpsd` handles LTO itself.

### Can the iPhone help?

- **Not over standard BLE.** An iPhone exposes ANCS, AMS and CTS to accessories, and none of them carries location. iOS doesn't offer the Location and Navigation Service (0x1819). There's no zero-install path.
- **Time already helps.** ioslink's CTS sync keeps the watch clock accurate, which the GNSS engine uses.
- **The only realistic BLE path is a small custom iOS app.**
  - It would be a CoreLocation + `CBPeripheralManager` app, serving the standard LNS "Location and Speed" characteristic (0x2A67) or a custom one, with background modes `location` and `bluetooth-peripheral`.
  - ioslink, already bonded and connected, subscribes and re-emits each position as `org.freedesktop.Geoclue.Position.PositionChanged` on the session bus. The provider then injects it automatically while acquiring. The same feed could also serve as a "connected GPS" source when the watch's GPS is off.
  - **Cost:** a few days of iOS work. It needs Xcode and an Apple ID; free signing expires every 7 days, a paid account doesn't. Background location also costs phone battery.
  - **Value is modest.** LTO already gives fast fixes. Worth it mainly for a "phone GPS" mode that saves watch battery.

## How to test

| # | What | Where | Needs |
|---|---|---|---|
| T0 | Baseline, read-only: nodes, `gpio42/43` = 1, `ttyBCM` IRQ count 0, no `gnss` in `lshal` (commands in [Kernel](#kernel-ready)) | anywhere | nothing |
| T1 | 3.1 indoors: `lhd` patch download, `chip.info`, NMEA with satellites but no fix | home | nothing installed |
| T2 | 3.1 outdoors: fix, TTFF, constellations, accuracy | outside | T1 |
| T3 | 3.2 + 3.3 outdoors: fixes through the HAL and geoclue | outside | one reboot, 3 packages |
| T4 | Screen-off session: fixes continue, the `geo` wakelock is held, battery % per 30 min | outside | T3 |
| T5 | Idle cost: today's unmanaged chip vs `lhd` idle | home, overnight | T1 |

**T2 (SSH doesn't reach the watch outdoors):** start a timed run on Wi-Fi, walk outside, then read the files back at home. Alternatively, put the watch on the iPhone's Personal Hotspot with a connman provisioning file like `odildo.config`, and SSH from the phone.

```sh
D=/home/root/gps-$(date +%m%d-%H%M); mkdir -p $D; date -u > $D/start
nohup sh -c "/vendor/bin/lhd /etc/gps/lhd.conf > $D/lhd.out 2>&1 &
  sleep 5; /vendor/bin/gpsd -c /etc/gps/gps.xml Periodic > $D/gpsd.out 2>&1 &
  for t in \$(seq 30); do [ -p /data/vendor/gps/gpspipe ] && break; sleep 1; done
  timeout 1200 cat /data/vendor/gps/gpspipe > $D/track.nmea
  killall gpsd lhd" > /dev/null 2>&1 &
# 20 min: stand still under open sky for 2-3 min first, then walk a known loop
```

Afterwards (copy `$D` to the Mac with `scp`):

```sh
grep -m1 -E '^\$G.GGA,[0-9.]*,[0-9]' track.nmea    # first fix; compare its UTC time with "start" = TTFF
grep -o -E '^\$G[A-Z]GSV' track.nmea | sort | uniq -c   # GP=GPS GL=GLONASS GA=Galileo GB/BD=BeiDou GQ=QZSS
grep -c -E '^\$G.GGA,[0-9.]*,[0-9]' track.nmea      # number of fixed epochs (gaps = the watch slept)
```

- **Optional GPX view:** `gpsbabel -i nmea -f track.nmea -o gpx -F track.gpx`, or any NMEA viewer. Compare against the iPhone's position at the start spot.
- **Starts to compare:**
  - **cold:** the first run ever, with no NV data or LTO yet;
  - **hot:** a restart a minute later;
  - **warm with LTO:** the next day, after a Wi-Fi session that fetched LTO.
- **T3:** the same timed approach, with the services from 3.2 started instead of the manual `lhd`/`gpsd`, and the test client as the consumer:
  ```sh
  nohup su ceres -c "XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus timeout 1200 /home/ceres/bin/geotest > /home/ceres/geo-$(date +%m%d-%H%M).txt 2>&1" > /dev/null 2>&1 &
  ```
- **T4:**
  - Read `cat /sys/power/wake_lock` during a session; expect `geo`.
  - Read `cat /sys/class/power_supply/battery/capacity` before and after.
  - Look for timestamp gaps with the screen off.
  - Check `journalctl -k | grep -i ttyBCM` for wake storms.
- **T5:** average `battery/current_now` over 5 min, screen off and the SSH session idle:
  ```sh
  for i in $(seq 300); do cat /sys/class/power_supply/battery/current_now; sleep 1; done | awk '{s+=$1} END {print s/NR " uA"}'
  ```
  - (a) today, with the chip unmanaged;
  - (b) with `echo 0 > /sys/bus/spi/devices/spi3.0/nstandby` (chip in standby; `echo 1` or a reboot undoes it);
  - (c) with `lhd` and `gpsd` idle.
  - Then one night each for (a) and (c). If (a) clearly exceeds (b), the always-out-of-standby chip costs battery today, and a boot-time `nstandby=0` is worth adding.

## Open questions

1. **Which exact part is it** (47755 or 47758, B1)? `chip.info` and the `lhd` log will say on the first run.
2. **Does `gpsd` run a job standalone** with `-c … Periodic` or the positional form, without a HAL client? T1 will tell.
3. **Does `gpsd` still try Android's SensorService with `SensorsEnable="false"`?** Logcat would show `Waited one second for android.frameworks.sensorservice@1.0::ISensorManager/default`.
4. **Does `gpsd`'s own LTO download resolve DNS** through connman's proxy (see [Assisted GPS](#assisted-gps))?
5. **What does the unmanaged chip cost at idle today,** and does a running `lhd` put it into a low-power state? (T5)
6. **Do fixes continue with the screen off,** and is the `geo` wakelock released after the session? (T4) Does the `ttyBCM` interrupt cause wakeups while `lhd` idles?
7. **Does `gpsd` accept more than one HAL client** (`test_gps` and the HIDL service at once)? Assume not, and run them one at a time.
8. **Is L5 usable?** Try `MultiCarrRFMode="GL_MULTI_CARR_RF_MODE_L1_L5"` in `/etc/gps/gps.xml`. It's a low-priority experiment; Mobvoi shipped L1 only.
9. **What did asteroidos.org's "partial" refer to?** Ask MagneFire or argosphil in #175.
10. **Later: the chip's hub can batch fixes and geofence** (`IGnssBatching`, FLP) while the main processor sleeps. That could make long workout tracking much cheaper, but the geoclue provider doesn't use it.
