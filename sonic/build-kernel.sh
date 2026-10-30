#!/bin/bash
# Runs INSIDE the sonic-kernel-builder container; start it with kbuild.sh.
#
# Builds one branch/commit of the kernel the way AsteroidOS's linux-rubyfish
# recipe does (meta-smartwatch 2.0, meta-oe linux.inc, oe-core kernel.bbclass,
# meta-asteroid mkboot.bbclass), then packs boot images with the 2.1 release's
# ramdisk, header values and cmdline. See BUILD.md for the reasoning.
#
# Mounts:   /repo  bare kernel repo (ro)     /work  volume: source tree
#           /ccache volume: ccache            /out   output root on the Mac
#           /sonic these scripts (ro)         /release/boot.img  2.1 boot image (ro)
# Env:      SONIC_REF       branch name (in /repo) or commit to build
#           SONIC_NAME      output directory name under /out
#           SONIC_BUILD_NO  build number, shown in `uname -v`
#           SONIC_MODE      trial (default) or repro (release's version strings)
#           SONIC_WORKTREE  1 = build /work/src as it is, no checkout or clean
#           SONIC_JOBS      parallel jobs
set -euo pipefail

RELEASE_SHA512=b834a219865371f88c991f63fbfd5e650ace6d038ea94348eac29a08f7a4d7e33314a3468d9fd3005e7ded4c3ba34f2ef1c2c312372df2d2f5d39ebabb593bf3
# Mobvoi commit AsteroidOS builds (SRCREV); its commit time is the release's
# KBUILD_BUILD_TIMESTAMP (Yocto's SOURCE_DATE_EPOCH).
BASE_COMMIT=c428ef3654d52e816308a6cf11009a1742f86c1c
TRIAL_ARGS="sonic.trial=1 msm_poweroff.download_mode=0"
DEFCONFIG=asteroidos-rubyfish_defconfig
# Appended device trees, in the release's order (rover first: the watch's
# bootloader reports androidboot.dtb_idx=0). The kernel's own zImage-dtb rule
# appends them in `find` order, which depends on the filesystem.
DTBS="arch/arm/boot/dts/qcom/rover.dtb arch/arm/boot/dts/qcom/rubyfish.dtb"

: "${SONIC_REF:?}" "${SONIC_NAME:?}" "${SONIC_BUILD_NO:?}"
SONIC_MODE=${SONIC_MODE:-trial}
SONIC_JOBS=${SONIC_JOBS:-$(nproc)}
out=/out/$SONIC_NAME
log() { printf '\n=== %s\n' "$*"; }

log "Checking the release boot image"
echo "$RELEASE_SHA512  /release/boot.img" | sha512sum -c -

log "Source tree"
src=/work/src
if [ ! -d "$src/.git" ]; then
    git clone --no-checkout /repo "$src"
fi
cd "$src"
if [ "${SONIC_WORKTREE:-0}" = 1 ]; then
    echo "building /work/src as it is (SONIC_WORKTREE=1)"
else
    if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
        git status --short --untracked-files=no
        echo "REFUSED: /work/src has uncommitted changes, which a checkout would throw away." >&2
        echo "Commit and push them, build them with --worktree, or discard them with" >&2
        echo "sonic/kshell.sh 'git reset --hard'." >&2
        exit 1
    fi
    git fetch --prune --quiet origin '+refs/heads/*:refs/remotes/origin/*'
    commit=$(git rev-parse --verify --quiet "origin/$SONIC_REF^{commit}" ||
             git rev-parse --verify "$SONIC_REF^{commit}")
    git checkout --quiet --force --detach "$commit"
    git clean -fdxq
fi
commit=$(git rev-parse HEAD)
short=$(git rev-parse --short=7 HEAD)
dirty=
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then dirty=-dirty; fi
git log -1 --format='%H %s'

# --- Environment, as in Yocto's kernel.bbclass / bitbake.conf -------------
export ARCH=arm CROSS_COMPILE=arm-linux-gnueabi-
export LC_ALL=en_US.UTF-8 TZ=UTC
export KBUILD_BUILD_USER=oe-user KBUILD_BUILD_HOST=oe-host
export KCONFIG_NOTIMESTAMP=1
export LOCALVERSION=          # set but empty: no "+" suffix on 4.9.232
case "$SONIC_MODE" in
    trial)
        export KBUILD_BUILD_VERSION="$SONIC_BUILD_NO-$short$dirty"
        export KBUILD_BUILD_TIMESTAMP="$(LC_ALL=C date -u)" ;;
    repro)
        export KBUILD_BUILD_VERSION=1
        export KBUILD_BUILD_TIMESTAMP="$(LC_ALL=C date -u -d "@$(git log -1 --format=%ct $BASE_COMMIT)")" ;;
    *) echo "unknown SONIC_MODE $SONIC_MODE" >&2; exit 2 ;;
esac
# Yocto's KERNEL_CC adds -fcanon-prefix-map (via DEBUG_PREFIX_MAP). It changes
# the kernel's own -fmacro-prefix-map=./= so __FILE__ becomes "/init/main.c"
# instead of "init/main.c", as in the release image.
CC="ccache ${CROSS_COMPILE}gcc -fcanon-prefix-map"
MAKE=(make -j"$SONIC_JOBS" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" CC="$CC" LOCALVERSION=)
echo "KBUILD_BUILD_VERSION=$KBUILD_BUILD_VERSION"
echo "KBUILD_BUILD_TIMESTAMP=$KBUILD_BUILD_TIMESTAMP"
${CROSS_COMPILE}gcc --version | head -n 1
${CROSS_COMPILE}ld --version | head -n 1

log "Configure ($DEFCONFIG)"
"${MAKE[@]}" "$DEFCONFIG"

log "Build zImage-dtb and modules"
start=$(date +%s)
"${MAKE[@]}" zImage-dtb modules
echo "build took $(( $(date +%s) - start )) s"
ccache --show-stats --verbose 2>/dev/null | grep -E 'Hits|Misses|Cache size' | head -n 4 || true

log "Assemble zImage-dtb in the release's DTB order"
built=$(cd arch/arm/boot/dts && find . -name '*.dtb' | sed 's|^\./|arch/arm/boot/dts/|' | sort | tr '\n' ' ')
want=$(echo $DTBS | tr ' ' '\n' | sort | tr '\n' ' ')
if [ "$built" != "$want" ]; then
    echo "unexpected set of device trees: $built (expected $want)" >&2
    exit 1
fi
rm -rf "$out"
mkdir -p "$out/modules"
cat arch/arm/boot/zImage $DTBS > "$out/zImage-dtb"
if cmp -s "$out/zImage-dtb" arch/arm/boot/zImage-dtb; then
    echo "kernel's own zImage-dtb has the same order"
else
    echo "kernel's own zImage-dtb used another DTB order (find order); using rover, rubyfish"
fi

log "Boot images (release ramdisk, header values and cmdline)"
python3 -I /sonic/bootimg.py unpack /release/boot.img /tmp/release >/dev/null
cmdline=$(cat /tmp/release/cmdline)
# Values from meta-rubyfish's img_info (the second offset is mkbootimg's
# default); verify.py checks every header field against the release image.
pack() {
    python3 -I /sonic/bootimg.py pack --kernel "$out/zImage-dtb" \
        --ramdisk /tmp/release/ramdisk --cmdline "$1" --board rubyfish \
        --base 0x80000000 --kernel_offset 0x00008000 \
        --ramdisk_offset 0x01000000 --second_offset 0x00f00000 \
        --tags_offset 0x00000100 --pagesize 2048 -o "$2"
}
pack "$cmdline $TRIAL_ARGS" "$out/boot-trial.img"   # for the recovery trial
pack "$cmdline" "$out/boot.img"                       # same kernel, for `boot` later

log "Copy build products to $out"
cp .config "$out/config"
cp System.map Module.symvers "$out/"
cp vmlinux "$out/vmlinux"
find . -name '*.ko' -exec cp {} "$out/modules/" \;
kernelrelease=$(cat include/config/kernel.release)
utsversion=$(sed -n 's/^#define UTS_VERSION "\(.*\)"/\1/p' include/generated/compile.h)
compiler=$(sed -n 's/^#define LINUX_COMPILER "\(.*\)"/\1/p' include/generated/compile.h)
{
    echo "ref:            $SONIC_REF"
    echo "commit:         $commit$dirty"
    echo "subject:        $(git log -1 --format=%s)"
    echo "mode:           $SONIC_MODE"
    echo "build number:   $SONIC_BUILD_NO"
    echo "uname -r:       $kernelrelease"
    echo "uname -v:       $utsversion"
    echo "compiler:       $compiler"
    echo "binutils:       $(${CROSS_COMPILE}ld --version | head -n 1)"
    echo "built:          $(LC_ALL=C date -u)"
    echo "trial cmdline:  $cmdline $TRIAL_ARGS"
    echo
    (cd "$out" && sha256sum zImage-dtb boot-trial.img boot.img config)
} > "$out/build-info.txt"
cat "$out/build-info.txt"

if [ "$kernelrelease" != 4.9.232 ]; then
    echo "kernel release is '$kernelrelease', expected 4.9.232" >&2
    exit 1
fi
