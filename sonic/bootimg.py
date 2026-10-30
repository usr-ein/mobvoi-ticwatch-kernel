#!/usr/bin/env python3
"""Inspect, split and compare Android v0 boot images and ARM zImage-dtb kernels.

Standard library only; runs on the Mac (python3 -I) or in the build container.

  bootimg.py info IMG                 print the header as JSON
  bootimg.py unpack IMG DIR           write kernel, ramdisk, header.json, and the
                                      kernel split into zImage + dtb-N-<model>.dtb
  bootimg.py split KERNEL DIR         split a zImage-dtb into zImage + DTBs
  bootimg.py pack -o OUT --kernel K --ramdisk R --cmdline C [--board ...]
                                      write a v0 boot image exactly like the
                                      mkbootimg AsteroidOS uses
  bootimg.py compare REF NEW          compare two boot images field by field;
                                      exit 1 if anything other than the kernel,
                                      its size, the id and the cmdline differ,
                                      or if NEW's cmdline is not REF's cmdline
                                      plus the trial arguments (see --extra)

Why our own packer: AsteroidOS packs with xiaolu/mkbootimg_tools (an AOSP
mkbootimg from 2015, meta-asteroid's mkbootimg-tools recipe). Current AOSP
mkbootimg (Debian's `mkbootimg` package) writes second_addr = 0 when there is
no second stage, where the old one wrote base + second_offset (0x80f00000).
`pack` reproduces the old tool; verify.py checks that it rebuilds the release
image byte for byte.
"""
import argparse
import hashlib
import json
import os
import struct
import sys

BOOT_MAGIC = b"ANDROID!"
FDT_MAGIC = 0xD00DFEED
ZIMAGE_MAGIC = 0x016F2818


def page_align(n, page):
    return (n + page - 1) // page * page


def parse_header(data):
    if data[:8] != BOOT_MAGIC:
        raise SystemExit("not an Android boot image (no ANDROID! magic)")
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size,
     second_addr, tags_addr, page_size, header_version,
     os_version) = struct.unpack_from("<10I", data, 8)
    name = data[48:64]
    cmdline = data[64:576]
    img_id = data[576:608]
    extra = data[608:1632]

    def cstr(b):
        return b.split(b"\0", 1)[0].decode("ascii", "replace")

    h = {
        "kernel_size": kernel_size,
        "kernel_addr": "0x%08x" % kernel_addr,
        "ramdisk_size": ramdisk_size,
        "ramdisk_addr": "0x%08x" % ramdisk_addr,
        "second_size": second_size,
        "second_addr": "0x%08x" % second_addr,
        "tags_addr": "0x%08x" % tags_addr,
        "page_size": page_size,
        "header_version": header_version,
        "os_version": os_version,
        "name": cstr(name),
        "cmdline": cstr(cmdline) + cstr(extra),
        "id": img_id.hex(),
    }
    # Offsets of the payloads (header occupies the first page).
    k_off = page_size
    r_off = k_off + page_align(kernel_size, page_size)
    s_off = r_off + page_align(ramdisk_size, page_size)
    end = s_off + page_align(second_size, page_size)
    h["_kernel_offset"] = k_off
    h["_ramdisk_offset"] = r_off
    h["_second_offset"] = s_off
    h["_image_end"] = end
    # Derived values (relative to base = kernel_addr - 0x8000, as mkbootimg does).
    base = kernel_addr - 0x8000
    h["_base_if_kernel_offset_0x8000"] = "0x%08x" % base
    h["_ramdisk_offset_from_base"] = "0x%08x" % (ramdisk_addr - base)
    h["_tags_offset_from_base"] = "0x%08x" % (tags_addr - base)
    h["_second_offset_from_base"] = "0x%08x" % ((second_addr - base) & 0xFFFFFFFF)
    # Raw header bytes that are not covered above must be zero for v0.
    h["_header_tail_nonzero"] = any(data[1632:page_size])
    return h


def payloads(data, h):
    k = data[h["_kernel_offset"]:h["_kernel_offset"] + h["kernel_size"]]
    r = data[h["_ramdisk_offset"]:h["_ramdisk_offset"] + h["ramdisk_size"]]
    s = data[h["_second_offset"]:h["_second_offset"] + h["second_size"]]
    return k, r, s


def expected_id(k, r, s):
    sha = hashlib.sha1()
    for blob in (k, r, s):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    return sha.digest().ljust(32, b"\0").hex()


def fdt_root_props(dtb):
    """Return the root node's properties of a flattened device tree."""
    magic, totalsize, off_struct, off_strings = struct.unpack_from(">4I", dtb, 0)
    if magic != FDT_MAGIC:
        raise ValueError("bad FDT magic")
    pos = off_struct
    depth = 0
    props = {}
    while True:
        (tok,) = struct.unpack_from(">I", dtb, pos)
        pos += 4
        if tok == 1:  # BEGIN_NODE
            end = dtb.index(b"\0", pos)
            pos = (end + 1 + 3) & ~3
            depth += 1
            if depth > 1:
                break
        elif tok == 2:  # END_NODE
            depth -= 1
            if depth == 0:
                break
        elif tok == 3:  # PROP
            ln, nameoff = struct.unpack_from(">2I", dtb, pos)
            pos += 8
            val = dtb[pos:pos + ln]
            pos = (pos + ln + 3) & ~3
            nend = dtb.index(b"\0", off_strings + nameoff)
            name = dtb[off_strings + nameoff:nend].decode()
            if depth == 1:
                props[name] = val
        elif tok == 4:  # NOP
            continue
        elif tok == 9:  # END
            break
        else:
            raise ValueError("bad FDT token %d" % tok)
    return props


def describe_dtb(dtb):
    p = fdt_root_props(dtb)
    out = {"size": len(dtb), "sha256": hashlib.sha256(dtb).hexdigest()}
    if "model" in p:
        out["model"] = p["model"].rstrip(b"\0").decode()
    if "compatible" in p:
        out["compatible"] = [c.decode() for c in p["compatible"].split(b"\0") if c]
    for key in ("qcom,msm-id", "qcom,board-id", "qcom,pmic-id"):
        if key in p:
            v = p[key]
            out[key] = ["0x%x" % x for x in struct.unpack(">%dI" % (len(v) // 4), v)]
    return out


def split_kernel(kernel):
    """Split zImage-dtb into (zImage, [dtb, ...]) using the zImage header."""
    magic, start, end = struct.unpack_from("<3I", kernel, 0x24)
    if magic != ZIMAGE_MAGIC:
        raise SystemExit("kernel is not an ARM zImage (magic 0x%08x)" % magic)
    zsize = end - start
    zimage = kernel[:zsize]
    dtbs = []
    pos = zsize
    while pos < len(kernel):
        magic, total = struct.unpack_from(">2I", kernel, pos)
        if magic != FDT_MAGIC:
            raise SystemExit("no FDT magic at offset %d after zImage" % pos)
        dtbs.append(kernel[pos:pos + total])
        pos += total
    if pos != len(kernel):
        raise SystemExit("appended DTBs overrun the kernel blob")
    # Cross-check against a plain search for the magic, as asked for.
    hits = []
    i = kernel.find(b"\xd0\x0d\xfe\xed")
    while i != -1:
        hits.append(i)
        i = kernel.find(b"\xd0\x0d\xfe\xed", i + 1)
    starts = []
    p = zsize
    for d in dtbs:
        starts.append(p)
        p += len(d)
    stray = [x for x in hits if x < zsize]
    return zimage, dtbs, {"zimage_size": zsize, "dtb_offsets": starts,
                          "magic_hits_inside_zimage": stray,
                          "magic_hits_total": len(hits)}


def safe_name(s):
    return "".join(c if c.isalnum() else "_" for c in s)[:60]


def kernel_banner(kernel):
    """The 'Linux version ...' string of a zImage(-dtb), or None."""
    import re
    import zlib
    try:
        zimage = split_kernel(kernel)[0]
    except SystemExit:
        return None
    idx = zimage.find(b"\x1f\x8b\x08")
    while idx >= 0:
        try:
            image = zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(zimage[idx:])
            m = re.search(rb"Linux version [^\n\0]*", image)
            if m:
                return m.group(0).decode(errors="replace")
        except zlib.error:
            pass
        idx = zimage.find(b"\x1f\x8b\x08", idx + 1)
    return None


def cmd_info(a):
    data = open(a.image, "rb").read()
    h = parse_header(data)
    k, r, s = payloads(data, h)
    h["_file_size"] = len(data)
    h["_id_matches_payloads"] = (expected_id(k, r, s) == h["id"])
    h["_kernel_sha256"] = hashlib.sha256(k).hexdigest()
    h["_ramdisk_sha256"] = hashlib.sha256(r).hexdigest()
    h["_trailing_bytes_after_payloads"] = len(data) - h["_image_end"]
    h["_kernel_banner"] = kernel_banner(k)
    print(json.dumps(h, indent=2))


def write_split(kernel, outdir):
    zimage, dtbs, info = split_kernel(kernel)
    open(os.path.join(outdir, "zImage"), "wb").write(zimage)
    info["dtbs"] = []
    for n, d in enumerate(dtbs):
        desc = describe_dtb(d)
        fn = "dtb-%d-%s.dtb" % (n, safe_name(desc.get("model", "unknown")))
        open(os.path.join(outdir, fn), "wb").write(d)
        desc["file"] = fn
        info["dtbs"].append(desc)
    info["zimage_sha256"] = hashlib.sha256(zimage).hexdigest()
    return info


def cmd_unpack(a):
    data = open(a.image, "rb").read()
    h = parse_header(data)
    k, r, s = payloads(data, h)
    os.makedirs(a.outdir, exist_ok=True)
    open(os.path.join(a.outdir, "kernel"), "wb").write(k)
    open(os.path.join(a.outdir, "ramdisk"), "wb").write(r)
    if s:
        open(os.path.join(a.outdir, "second"), "wb").write(s)
    open(os.path.join(a.outdir, "cmdline"), "w").write(h["cmdline"])
    h["split"] = write_split(k, a.outdir)
    json.dump(h, open(os.path.join(a.outdir, "header.json"), "w"), indent=2)
    print(json.dumps(h, indent=2))


def cmd_split(a):
    kernel = open(a.kernel, "rb").read()
    os.makedirs(a.outdir, exist_ok=True)
    print(json.dumps(write_split(kernel, a.outdir), indent=2))


def pack_v0(kernel, ramdisk, cmdline, board, base, kernel_offset, ramdisk_offset,
            second_offset, tags_offset, page_size):
    """Byte-for-byte the algorithm of xiaolu/mkbootimg_tools' mkbootimg (no second stage)."""
    if len(board) > 16 or len(cmdline) > 1536:
        raise SystemExit("board name or cmdline too long")
    if page_size & (page_size - 1):
        raise SystemExit("page size must be a power of two")
    out = bytearray()
    out += struct.pack("<8s8I", BOOT_MAGIC, len(kernel), base + kernel_offset,
                       len(ramdisk), base + ramdisk_offset, 0, base + second_offset,
                       base + tags_offset, page_size)
    out += bytes(8)                                  # header_version, os_version = 0
    out += struct.pack("16s", board.encode())
    out += struct.pack("512s", cmdline[:512].encode())
    out += bytes.fromhex(expected_id(kernel, ramdisk, b""))
    out += struct.pack("1024s", cmdline[512:].encode())

    def pad():
        out.extend(bytes((page_size - len(out) % page_size) % page_size))

    pad()
    out += kernel
    pad()
    out += ramdisk
    pad()
    return bytes(out)


def cmd_pack(a):
    data = pack_v0(open(a.kernel, "rb").read(), open(a.ramdisk, "rb").read(), a.cmdline,
                   a.board, a.base, a.kernel_offset, a.ramdisk_offset, a.second_offset,
                   a.tags_offset, a.pagesize)
    open(a.output, "wb").write(data)


def cmd_compare(a):
    ref = open(a.ref, "rb").read()
    new = open(a.new, "rb").read()
    hr, hn = parse_header(ref), parse_header(new)
    kr, rr, sr = payloads(ref, hr)
    kn, rn, sn = payloads(new, hn)
    problems, notes = [], []
    same_fields = ["kernel_addr", "ramdisk_size", "ramdisk_addr", "second_size",
                   "second_addr", "tags_addr", "page_size", "header_version",
                   "os_version", "name", "_header_tail_nonzero"]
    for f in same_fields:
        if hr[f] != hn[f]:
            problems.append("header field %s differs: %r -> %r" % (f, hr[f], hn[f]))
    if rr != rn:
        problems.append("ramdisk differs")
    else:
        notes.append("ramdisk identical (%d bytes, sha256 %s)" % (len(rr), hashlib.sha256(rr).hexdigest()))
    if sr != sn:
        problems.append("second-stage payload differs")
    want = hr["cmdline"] + ((" " + a.extra) if a.extra else "")
    if hn["cmdline"] != want:
        problems.append("cmdline is not the reference cmdline + %r:\n  ref: %s\n  new: %s"
                        % (a.extra, hr["cmdline"], hn["cmdline"]))
    else:
        notes.append("cmdline = reference cmdline + %r" % a.extra)
    if expected_id(kn, rn, sn) != hn["id"]:
        problems.append("new image id field does not match its payloads")
    if any(new[hn["_image_end"]:]):
        problems.append("new image has non-zero bytes after its payloads")
    # DTBs: must be byte-identical and in the same order.
    _, dr, _ = split_kernel(kr)
    _, dn, _ = split_kernel(kn)
    if len(dr) != len(dn):
        problems.append("number of appended DTBs differs: %d -> %d" % (len(dr), len(dn)))
    for i, (x, y) in enumerate(zip(dr, dn)):
        if x != y:
            problems.append("appended DTB #%d differs (%s vs %s)" % (
                i, describe_dtb(x).get("model"), describe_dtb(y).get("model")))
        else:
            notes.append("appended DTB #%d identical: %s, %d bytes" % (
                i, describe_dtb(x).get("model"), len(x)))
    notes.append("kernel size %d -> %d; image size %d -> %d" % (
        len(kr), len(kn), hr["_image_end"], hn["_image_end"]))
    for n in notes:
        print("ok:   " + n)
    for p in problems:
        print("FAIL: " + p)
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info")
    p.add_argument("image")
    p.set_defaults(fn=cmd_info)
    p = sub.add_parser("unpack")
    p.add_argument("image")
    p.add_argument("outdir")
    p.set_defaults(fn=cmd_unpack)
    p = sub.add_parser("split")
    p.add_argument("kernel")
    p.add_argument("outdir")
    p.set_defaults(fn=cmd_split)
    p = sub.add_parser("pack")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--kernel", required=True)
    p.add_argument("--ramdisk", required=True)
    p.add_argument("--cmdline", default="")
    p.add_argument("--board", default="")
    num = lambda x: int(x, 0)  # noqa: E731
    p.add_argument("--base", type=num, default=0x10000000)
    p.add_argument("--kernel_offset", type=num, default=0x00008000)
    p.add_argument("--ramdisk_offset", type=num, default=0x01000000)
    p.add_argument("--second_offset", type=num, default=0x00f00000)
    p.add_argument("--tags_offset", type=num, default=0x00000100)
    p.add_argument("--pagesize", type=num, default=2048)
    p.set_defaults(fn=cmd_pack)
    p = sub.add_parser("compare")
    p.add_argument("ref")
    p.add_argument("new")
    p.add_argument("--extra", default="",
                   help="arguments expected to be appended to REF's cmdline")
    p.set_defaults(fn=cmd_compare)
    a = ap.parse_args()
    sys.exit(a.fn(a) or 0)


if __name__ == "__main__":
    main()
