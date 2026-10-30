# Build environment for the AsteroidOS 2.1 "rubyfish" kernel (TicWatch Pro 3
# Ultra, codename rover). See sonic/BUILD.md.
#
# AsteroidOS 2.1 (Yocto walnascar) built the kernel with GCC 14.3.0 and
# binutils 2.44 ("gcc version 14.3.0 (GCC)" in /proc/version). We use the same
# versions, unpatched: kernel.org's crosstool "nolibc" build, made for kernel
# builds (https://mirrors.edge.kernel.org/pub/tools/crosstool/). Its target is
# arm-linux-gnueabi; the float ABI default does not matter for the kernel,
# which is always compiled with -msoft-float.

# debian:trixie, pinned by digest so the host tools (gzip, mkbootimg, ...) stay fixed.
FROM debian:trixie@sha256:2dc419c6d7fe3dad0a2f9fd469c34cc90b0f9ba642d8fde837fe3d7d34a0e7f1

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      bc bison flex make gcc libc6-dev libssl-dev openssl perl python3 \
      git ca-certificates curl xz-utils cpio kmod file gzip locales \
      device-tree-compiler mkbootimg ccache \
 && rm -rf /var/lib/apt/lists/* \
 && sed -i 's/^# *\(en_US.UTF-8 UTF-8\)/\1/' /etc/locale.gen \
 && locale-gen \
 && git config --system --add safe.directory '*'
# Yocto builds with LC_ALL=en_US.UTF-8. It matters: `nm -n` breaks ties
# between symbols at the same address with strcoll(), which changes the
# kallsyms table. build-kernel.sh sets it.

# GCC 14.3.0 + binutils 2.44 cross toolchain. The hashes are the ones in
# kernel.org's signed sha256sums.asc for these files.
ARG TARGETARCH
RUN set -eu; \
    case "${TARGETARCH:-arm64}" in \
      arm64) host=arm64;  sha=4c441cc512322b8561973f94b4f0a611c0302ee494553cafa41de484cf6eb449 ;; \
      amd64) host=x86_64; sha=7c0c5cc1014b71c8ae4735f1c3b6573965acc2c46e5a928994226d4958ef99c7 ;; \
      *) echo "unsupported build host architecture: $TARGETARCH" >&2; exit 1 ;; \
    esac; \
    f="$host-gcc-14.3.0-nolibc-arm-linux-gnueabi.tar.xz"; \
    curl -fsSLo "/tmp/$f" "https://mirrors.edge.kernel.org/pub/tools/crosstool/files/bin/$host/14.3.0/$f"; \
    echo "$sha  /tmp/$f" | sha256sum -c -; \
    tar -C /opt -xJf "/tmp/$f"; \
    rm "/tmp/$f"; \
    /opt/gcc-14.3.0-nolibc/arm-linux-gnueabi/bin/arm-linux-gnueabi-gcc --version | head -n 1

ENV PATH=/opt/gcc-14.3.0-nolibc/arm-linux-gnueabi/bin:$PATH \
    ARCH=arm \
    CROSS_COMPILE=arm-linux-gnueabi- \
    CCACHE_DIR=/ccache \
    CCACHE_MAXSIZE=10G \
    CCACHE_COMPILERCHECK=content \
    LANG=C.UTF-8 \
    TZ=UTC

WORKDIR /work
