#!/bin/bash
# Build a branch (or commit) of the kernel in Docker, then verify it against
# the AsteroidOS 2.1 release. Runs on the Mac. See BUILD.md.
#
#   sonic/kbuild.sh [--repro] [--worktree] [--no-verify] [--allow-config-diff] BRANCH-OR-COMMIT
#
#   --repro      use the release's version strings (#1, Nov 3 2021) instead of
#                a build number and the current time, to compare the kernel
#                byte for byte with the release. Never flash a repro build.
#   --worktree   build the container's /work/src as it is (uncommitted edits
#                included); `uname -v` then ends in -dirty
#   --no-verify  skip verify.py
#   --allow-config-diff  for branches that change the kernel config on purpose:
#                verify.py prints the config diff instead of failing on it
#
# Output: $SONIC_OUT/NNN-BRANCH/ with boot-trial.img, boot.img, zImage-dtb,
# config, System.map, Module.symvers, vmlinux, modules/, build-info.txt and
# verify.txt.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
REPO=${SONIC_REPO:-$HOME/Documents/mobvoi-ticwatch-kernel.git}
OUT_ROOT=${SONIC_OUT:-$HOME/Documents/sonic-screwdriver/build/kernel}
RELEASE=${SONIC_RELEASE_IMG:-$HOME/Downloads/asteroidos-rubyfish-2.1/asteroid-rubyfish-boot.img}
IMAGE=sonic-kernel-builder
mode=trial worktree=0 verify=1 verify_args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --repro) mode=repro ;;
        --worktree) worktree=1 ;;
        --no-verify) verify=0 ;;
        --allow-config-diff) verify_args+=(--allow-config-diff) ;;
        -h|--help) sed -n '2,/^[^#]/{/^#/p;}' "$0"; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
    shift
done
ref=${1:?usage: kbuild.sh [--repro] [--worktree] [--no-verify] [--allow-config-diff] BRANCH-OR-COMMIT}

[ -d "$REPO" ] || { echo "no kernel repo at $REPO (set SONIC_REPO)" >&2; exit 1; }
[ -f "$RELEASE" ] || { echo "no release boot image at $RELEASE (set SONIC_RELEASE_IMG)" >&2; exit 1; }
mkdir -p "$OUT_ROOT"

# The image is cheap to rebuild from cache, and this keeps it in step with the Dockerfile.
docker build -q -t "$IMAGE" "$here" >/dev/null

n=$(( $(cat "$OUT_ROOT/.build-number" 2>/dev/null || echo 0) + 1 ))
echo "$n" > "$OUT_ROOT/.build-number"
safe_ref=$(printf '%s' "$ref" | tr -c 'A-Za-z0-9._-' '_')
name=$(printf '%03d-%s' "$n" "$safe_ref")
[ "$mode" = repro ] && name=$name-repro
echo "build $n: $ref ($mode) -> $OUT_ROOT/$name"

docker run --rm \
    -v sonic-kernel-work:/work \
    -v sonic-kernel-ccache:/ccache \
    -v "$REPO":/repo:ro \
    -v "$RELEASE":/release/boot.img:ro \
    -v "$here":/sonic:ro \
    -v "$OUT_ROOT":/out \
    -e SONIC_REF="$ref" -e SONIC_NAME="$name" -e SONIC_BUILD_NO="$n" \
    -e SONIC_MODE="$mode" -e SONIC_WORKTREE="$worktree" \
    "$IMAGE" /sonic/build-kernel.sh 2>&1 | tee "$OUT_ROOT/$name.log" || {
    echo "BUILD FAILED, log: $OUT_ROOT/$name.log" >&2
    exit 1
}
mv "$OUT_ROOT/$name.log" "$OUT_ROOT/$name/build.log"

if [ "$verify" = 1 ]; then
    docker run --rm \
        -v "$RELEASE":/release/boot.img:ro \
        -v "$here":/sonic:ro \
        -v "$OUT_ROOT/$name":/build \
        "$IMAGE" python3 -I /sonic/verify.py ${verify_args[@]+"${verify_args[@]}"} /release/boot.img /build \
        | tee "$OUT_ROOT/$name/verify.txt" || {
        echo "VERIFY FAILED, see $OUT_ROOT/$name/verify.txt" >&2
        exit 1
    }
fi
echo
echo "output: $OUT_ROOT/$name"
