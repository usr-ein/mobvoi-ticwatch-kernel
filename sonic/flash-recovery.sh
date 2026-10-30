#!/bin/bash
# Write a TRIAL boot image to the watch's recovery partition over SSH, read it
# back, and (with --reboot) boot it once. Runs on the Mac. See BUILD.md.
#
#   sonic/flash-recovery.sh [--reboot] [--boot-not-release] BUILD_DIR/boot-trial.img
#   sonic/flash-recovery.sh --control [--reboot] control/boot-release-trial.img
#
# Refuses unless:
#   - the image is an Android boot image whose cmdline has the trial arguments
#     (sonic.trial=1 msm_poweroff.download_mode=0), so the dead-man's switch
#     applies and a panic reboots instead of entering download mode;
#   - the dead-man's switch (kernel-trial.service) is installed and enabled;
#   - the image fits the recovery partition;
#   - `boot` still holds the AsteroidOS 2.1 release kernel, i.e. a known-good
#     kernel to fall back to (override with --boot-not-release only if you know
#     what is in boot and that it boots);
#   - the kernel is not the release's (uname -v must show a trial). --control
#     lifts this one check, for the control image of BUILD.md (the release
#     kernel byte for byte, with the trial arguments), used to tell effects of
#     booting from recovery apart from effects of a new kernel.
# It never writes anything but the recovery partition and /tmp/sonic-trial.img.
set -euo pipefail

HOST=${SONIC_HOST:-sonic-screwdriver}
BYNAME=/dev/block/platform/msm_sdcc.1/by-name
RELEASE_SHA512=b834a219865371f88c991f63fbfd5e650ace6d038ea94348eac29a08f7a4d7e33314a3468d9fd3005e7ded4c3ba34f2ef1c2c312372df2d2f5d39ebabb593bf3
RELEASE_SIZE=13758464
here=$(cd "$(dirname "$0")" && pwd)
reboot=0 boot_any=0 control=0
while [ $# -gt 0 ]; do
    case "$1" in
        --reboot) reboot=1 ;;
        --boot-not-release) boot_any=1 ;;
        --control) control=1 ;;
        -h|--help) sed -n '2,/^[^#]/{/^#/p;}' "$0"; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
    shift
done
img=${1:?usage: flash-recovery.sh [--reboot] BUILD_DIR/boot-trial.img}
ssh_() { ssh -o ConnectTimeout=20 -o ServerAliveInterval=10 "$HOST" "$@"; }

echo "== local checks: $img"
info=$(python3 -I "$here/bootimg.py" info "$img")
printf '%s\n' "$info" | grep -q '"cmdline": ".* sonic.trial=1 msm_poweroff.download_mode=0' \
    || { echo "REFUSED: cmdline lacks the trial arguments" >&2; exit 1; }
printf '%s\n' "$info" | grep -q '"_id_matches_payloads": true' \
    || { echo "REFUSED: header id does not match the payloads" >&2; exit 1; }
banner=$(printf '%s\n' "$info" | sed -n 's/.*"_kernel_banner": "\(.*\)",\{0,1\}$/\1/p')
echo "kernel: $banner"
case "$banner" in
    *"#1 SMP PREEMPT Wed Nov 3 10:42:30 UTC 2021"*)
        if [ "$control" = 1 ]; then
            echo "control image: release version string accepted (--control)"
        else
            echo "REFUSED: this kernel carries the release's version string (a --repro build?);" \
                 "a trial must be distinguishable in uname -v" >&2; exit 1
        fi ;;
    "Linux version 4.9.232 "*) ;;
    *) echo "REFUSED: not a 4.9.232 kernel" >&2; exit 1 ;;
esac
size=$(wc -c < "$img" | tr -d ' ')
sha=$(shasum -a 256 "$img" | cut -d' ' -f1)
echo "size $size, sha256 $sha"

echo "== watch checks ($HOST)"
remote=$(ssh_ "set -e
    r=\$(readlink -f $BYNAME/recovery); b=\$(readlink -f $BYNAME/boot)
    echo \"recovery=\$r boot=\$b\"
    echo \"recovery_size=\$(( \$(cat /sys/class/block/\${r##*/}/size) * 512 ))\"
    echo \"boot_sha512=\$(head -c $RELEASE_SIZE $BYNAME/boot | sha512sum | cut -d' ' -f1)\"
    echo \"switch=\$(systemctl is-enabled kernel-trial.service 2>/dev/null || true)\"
    echo \"on_trial=\$(grep -c 'sonic.trial=1' /proc/cmdline || true)\"")
echo "$remote"
val() { printf '%s\n' "$remote" | sed -n "s/^$1=\([^ ]*\).*/\1/p"; }
rdev=$(val recovery); bdev=$(printf '%s\n' "$remote" | sed -n 's/.* boot=\(.*\)/\1/p')
[ -n "$rdev" ] && [ "$rdev" != "$bdev" ] || { echo "REFUSED: cannot tell recovery from boot" >&2; exit 1; }
[ "$(val switch)" = enabled ] || { echo "REFUSED: kernel-trial.service is not enabled on the watch" >&2; exit 1; }
[ "$size" -le "$(val recovery_size)" ] || { echo "REFUSED: image larger than recovery" >&2; exit 1; }
if [ "$(val boot_sha512)" = "$RELEASE_SHA512" ]; then
    echo "boot holds the AsteroidOS 2.1 release kernel (known good)"
elif [ "$boot_any" = 1 ]; then
    echo "WARNING: boot does not hold the 2.1 release kernel (--boot-not-release given)"
else
    echo "REFUSED: boot does not hold the 2.1 release kernel; see BUILD.md" >&2; exit 1
fi

echo "== copy to /tmp/sonic-trial.img"
ssh_ "cat > /tmp/sonic-trial.img" < "$img"
rsha=$(ssh_ "sha256sum /tmp/sonic-trial.img | cut -d' ' -f1")
[ "$rsha" = "$sha" ] || { echo "copy corrupted ($rsha)" >&2; exit 1; }

echo "== write recovery ($rdev) and read it back"
ssh_ "dd if=/tmp/sonic-trial.img of=$BYNAME/recovery bs=1M conv=fsync status=none && sync"
back=$(ssh_ "head -c $size $BYNAME/recovery | sha256sum | cut -d' ' -f1")
if [ "$back" != "$sha" ]; then
    echo "READ-BACK MISMATCH ($back): recovery is not what was written; do NOT reboot into recovery" >&2
    exit 1
fi
ssh_ "rm -f /tmp/sonic-trial.img"
echo "recovery now holds $img (sha256 $sha, $size bytes)"

if [ "$reboot" = 1 ]; then
    echo "== systemctl reboot --reboot-argument=recovery"
    ssh_ "systemctl reboot --reboot-argument=recovery" || true
    echo "rebooting; the watch is back on SSH in about 30-60 s. Then run sonic/trial-check.sh."
else
    echo "next: ssh $HOST systemctl reboot --reboot-argument=recovery"
fi
