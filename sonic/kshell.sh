#!/bin/bash
# Shell (or one command) in the build container, in the kernel source tree
# /work/src of the sonic-kernel-work volume. Runs on the Mac. See BUILD.md.
#
#   sonic/kshell.sh                 interactive shell
#   sonic/kshell.sh 'git status'    run a command (bash -c) and exit
#
# The bare repo is mounted read-write at /repo (the tree's `origin`), so
# `git push origin HEAD:BRANCH` publishes a commit to it. With SONIC_MAC_DIR
# set, that Mac directory is mounted at /mac, to move files to and from an
# editor on the Mac. Never check the tree out on the Mac itself.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
REPO=${SONIC_REPO:-$HOME/Documents/mobvoi-ticwatch-kernel.git}
IMAGE=sonic-kernel-builder
docker build -q -t "$IMAGE" "$here" >/dev/null
mac=()
if [ -n "${SONIC_MAC_DIR:-}" ]; then
    mkdir -p "$SONIC_MAC_DIR"
    mac=(-v "$(cd "$SONIC_MAC_DIR" && pwd)":/mac)
fi
tty=()
[ -t 0 ] && [ -t 1 ] && tty=(-it)
init='[ -d /work/src/.git ] || git clone -q --no-checkout /repo /work/src; cd /work/src'
if [ $# -eq 0 ]; then
    cmd="$init; exec bash"
else
    cmd="$init; $1"
fi
exec docker run --rm ${tty[@]+"${tty[@]}"} \
    -v sonic-kernel-work:/work \
    -v sonic-kernel-ccache:/ccache \
    -v "$REPO":/repo \
    ${mac[@]+"${mac[@]}"} \
    "$IMAGE" bash -c "$cmd"
