#!/usr/bin/env python3
"""Check a kernel build against the AsteroidOS 2.1 release before it goes near the watch.

  verify.py RELEASE_BOOT_IMG BUILD_DIR [--recovery-size BYTES] [--allow-config-diff]

BUILD_DIR is a kbuild.sh output directory. Exit status 0 means every hard
check passed. Hard checks:

  1. the boot image packer reproduces the release image byte for byte;
  2. BUILD_DIR/config equals the release kernel's embedded config (IKCONFIG,
     i.e. the watch's /proc/config.gz), and so does the config embedded in the
     new kernel (skipped with --allow-config-diff, for branches that change
     the config on purpose; the diff is printed either way);
  3. both boot images keep the release's header values and ramdisk; boot.img
     has the release cmdline, boot-trial.img the release cmdline plus the
     trial arguments; the appended DTBs are byte-identical to the release's,
     in the same order;
  4. boot-trial.img fits the recovery partition;
  5. the kernel release string is 4.9.232.

Reported, not enforced: whether the decompressed kernel Image is identical to
the release's (it is for a --repro build of asteroidos-2.1), and whether the
exported symbol CRCs (MODVERSIONS) match the release's.
"""
import argparse
import difflib
import hashlib
import os
import re
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bootimg  # noqa: E402

TRIAL_ARGS = "sonic.trial=1 msm_poweroff.download_mode=0"
RECOVERY_SIZE = 32 * 1024 * 1024
KERNEL_BASE = 0xC0008000  # PAGE_OFFSET (VMSPLIT_3G) + TEXT_OFFSET

failures = []


def ok(msg):
    print("ok:   " + msg)


def fail(msg):
    print("FAIL: " + msg)
    failures.append(msg)


def info(msg):
    print("info: " + msg)


def kernel_of(img_path):
    data = open(img_path, "rb").read()
    h = bootimg.parse_header(data)
    k, r, s = bootimg.payloads(data, h)
    return h, k, r


def decompress_zimage(zimage):
    """Return the decompressed kernel Image inside an ARM zImage (gzip piggy)."""
    idx = 0
    while True:
        idx = zimage.find(b"\x1f\x8b\x08", idx)
        if idx < 0:
            raise SystemExit("no gzip stream in zImage")
        try:
            d = zlib.decompressobj(16 + zlib.MAX_WBITS)
            img = d.decompress(zimage[idx:])
            if len(img) > 1_000_000:
                return img
        except zlib.error:
            pass
        idx += 1


def ikconfig(image):
    start = image.find(b"IKCFG_ST")
    end = image.find(b"IKCFG_ED", start)
    if start < 0 or end < 0:
        return None
    return zlib.decompress(image[start + 8:end], 16 + zlib.MAX_WBITS)


def banner(image):
    m = re.search(rb"Linux version [^\n\0]*", image)
    return m.group(0).decode(errors="replace") if m else None


# --- exported symbol CRCs ----------------------------------------------------
# __ksymtab* is an array of {u32 value, u32 name}; __kcrctab* follows with one
# u32 per symbol in the same order. Find the longest run of entries whose name
# pointer points at an identifier string, then read as many CRCs after it.

def _is_ident_at(img, off):
    if off <= 0 or off >= len(img) - 1 or img[off - 1] != 0:
        return False
    end = img.find(b"\0", off, off + 200)
    if end <= off:
        return False
    return all(c == 0x5F or 0x30 <= c <= 0x39 or 0x41 <= c <= 0x5A or 0x61 <= c <= 0x7A
               for c in img[off:end])


def export_crcs(img):
    n = len(img) // 4
    words = struct.unpack("<%dI" % n, img[:n * 4])
    lo, hi = KERNEL_BASE, KERNEL_BASE + len(img)
    best = (0, 0)
    for parity in (0, 1):
        run_len, run_start, j = 0, 0, parity
        while j + 1 < n:
            p = words[j + 1]
            if lo <= p < hi and _is_ident_at(img, p - KERNEL_BASE):
                if run_len == 0:
                    run_start = j
                run_len += 1
                if run_len > best[1]:
                    best = (run_start, run_len)
            else:
                run_len = 0
            j += 2
    start, count = best
    if count < 100:
        return None
    ptrs = [words[start + 2 * k + 1] for k in range(count)]
    s_lo, s_hi = min(ptrs) - 0x40000, max(ptrs) + 0x40000
    while start >= 2 and s_lo <= words[start - 1] < s_hi:
        start -= 2
        count += 1
    while start + 2 * count + 1 < n and s_lo <= words[start + 2 * count + 1] < s_hi:
        count += 1
    names = []
    for k in range(count):
        off = words[start + 2 * k + 1] - KERNEL_BASE
        names.append(img[off:img.find(b"\0", off)].decode(errors="replace"))
    crcs = words[start + 2 * count:start + 3 * count]
    return dict(zip(names, crcs))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("release")
    ap.add_argument("build")
    ap.add_argument("--recovery-size", type=int, default=RECOVERY_SIZE)
    ap.add_argument("--allow-config-diff", action="store_true")
    a = ap.parse_args()

    rel_h, rel_k, rel_r = kernel_of(a.release)
    trial = os.path.join(a.build, "boot-trial.img")
    plain = os.path.join(a.build, "boot.img")
    print("release: %s" % a.release)
    print("build:   %s" % a.build)
    bi = os.path.join(a.build, "build-info.txt")
    if os.path.exists(bi):
        for line in open(bi):
            if line.startswith(("ref:", "commit:", "mode:", "uname -v:")):
                print("         " + line.rstrip())
    print()

    # 1. packer self-test --------------------------------------------------
    print("[1] boot image packer")
    repacked = bootimg.pack_v0(rel_k, rel_r, rel_h["cmdline"], "rubyfish", 0x80000000,
                               0x8000, 0x01000000, 0x00f00000, 0x100, 2048)
    if repacked == open(a.release, "rb").read():
        ok("bootimg.py pack rebuilds the release image byte for byte from its kernel, ramdisk and cmdline")
    else:
        fail("bootimg.py pack does not reproduce the release image")
    print()

    # 2. config ---------------------------------------------------------------
    print("[2] kernel config")
    rel_image = decompress_zimage(bootimg.split_kernel(rel_k)[0])
    rel_cfg = ikconfig(rel_image)
    new_k = open(os.path.join(a.build, "zImage-dtb"), "rb").read()
    new_image = decompress_zimage(bootimg.split_kernel(new_k)[0])
    new_cfg_embedded = ikconfig(new_image)
    new_cfg = open(os.path.join(a.build, "config"), "rb").read()
    if new_cfg_embedded != new_cfg:
        fail("config embedded in the new kernel differs from BUILD_DIR/config")
    if rel_cfg == new_cfg:
        ok("config identical to the release kernel's /proc/config.gz (%d bytes, header included)"
           % len(new_cfg))
    else:
        diff = list(difflib.unified_diff(rel_cfg.decode().splitlines(),
                                         new_cfg.decode().splitlines(),
                                         "release /proc/config.gz", "build config", lineterm="", n=0))
        changed = [l for l in diff if l[:1] in "+-" and not l.startswith(("+++", "---"))]
        body = [l for l in changed if not l[1:].startswith("#")]
        (info if a.allow_config_diff else fail)(
            "config differs from the release (%d changed lines, %d of them settings):" % (len(changed), len(body)))
        for l in diff:
            print("      " + l)
    print()

    # 3. boot images ------------------------------------------------------------
    for path, extra in ((trial, TRIAL_ARGS), (plain, "")):
        print("[3] %s vs release" % os.path.basename(path))
        cmd = [sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bootimg.py"),
               "compare", a.release, path, "--extra", extra]
        res = subprocess.run(cmd, capture_output=True, text=True)
        for line in res.stdout.splitlines():
            print("  " + line)
        if res.returncode != 0:
            fail("%s does not match the release where it must" % os.path.basename(path))
        print()

    # 4. size -------------------------------------------------------------------
    print("[4] size")
    size = os.path.getsize(trial)
    if size <= a.recovery_size:
        ok("boot-trial.img is %d bytes, recovery partition %d bytes (%.1f%% used)"
           % (size, a.recovery_size, 100.0 * size / a.recovery_size))
    else:
        fail("boot-trial.img (%d bytes) does not fit the recovery partition (%d)" % (size, a.recovery_size))
    print()

    # 5. kernel release, banner, Image comparison ----------------------------------
    print("[5] kernel image")
    nb, rb = banner(new_image), banner(rel_image)
    info("release: %s" % rb)
    info("build:   %s" % nb)
    m = re.match(r"Linux version (\S+) ", nb or "")
    if m and m.group(1) == "4.9.232":
        ok("kernel release string is 4.9.232")
    else:
        fail("kernel release string is %r, expected 4.9.232" % (m.group(1) if m else None))
    if new_image == rel_image:
        ok("decompressed kernel Image is byte-identical to the release's (%d bytes)" % len(new_image))
    elif len(new_image) == len(rel_image):
        diffs = [i for i, (x, y) in enumerate(zip(new_image, rel_image)) if x != y]
        note = b"\x04\0\0\0\x14\0\0\0\x03\0\0\0GNU\0"   # NT_GNU_BUILD_ID, 20-byte SHA-1
        d0 = diffs[0]
        if diffs[-1] < d0 + 20 and rel_image[d0 - 16:d0] == note:
            nd = rel_image.find(note) + 16
            ok("decompressed kernel Image (%d bytes) is identical to the release's except the "
               "20-byte GNU build ID at offset 0x%x (a hash over vmlinux including its debug info, "
               "which holds build paths)" % (len(new_image), nd))
        else:
            info("decompressed kernel Image: same size as the release's, %d bytes differ "
                 "(first at offset 0x%x)" % (len(diffs), d0))
    else:
        info("decompressed kernel Image differs from the release's: %d vs %d bytes"
             % (len(new_image), len(rel_image)))
        info("(expected unless this is a --repro build of the release source: the version "
             "string alone moves everything after it)")
    zr, zn = bootimg.split_kernel(rel_k)[0], bootimg.split_kernel(new_k)[0]
    info("zImage: %s (%d bytes, sha256 %s)" % ("identical to the release's" if zr == zn else "differs from the release's",
                                             len(zn), hashlib.sha256(zn).hexdigest()[:16]))
    print()

    # 6. exported symbol CRCs -------------------------------------------------------
    print("[6] exported symbol CRCs (MODVERSIONS)")
    cr, cn = export_crcs(rel_image), export_crcs(new_image)
    if not cr or not cn:
        info("could not locate the export tables; skipped")
    else:
        common = set(cr) & set(cn)
        mism = sorted(s for s in common if cr[s] != cn[s])
        only_r, only_n = sorted(set(cr) - set(cn)), sorted(set(cn) - set(cr))
        msg = "%d exports in release, %d in build, %d CRC mismatches, %d removed, %d added" % (
            len(cr), len(cn), len(mism), len(only_r), len(only_n))
        if not mism and not only_r and not only_n:
            ok(msg + " (module ABI unchanged)")
        else:
            info(msg)
            for s in mism[:20]:
                info("  CRC changed: %s" % s)
            for s in only_r[:20]:
                info("  removed: %s" % s)
            for s in only_n[:20]:
                info("  added: %s" % s)
    print()

    if failures:
        print("RESULT: %d check(s) FAILED" % len(failures))
        return 1
    print("RESULT: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
