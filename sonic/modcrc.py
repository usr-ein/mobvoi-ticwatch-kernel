#!/usr/bin/env python3
"""Check that kernel modules built for another kernel pass MODVERSIONS on a build.

  modcrc.py KERNEL MODULE_DIR [--all]

KERNEL is a boot image, a zImage-dtb or a kbuild.sh output directory.
MODULE_DIR holds the .ko files, e.g. the watch's /vendor/lib/modules (the 25
stock audio_*.ko modules; never commit them).

Every entry of every module's __versions table is checked the way
kernel/module.c does when the module loads: a symbol that one of the modules
in MODULE_DIR exports is resolved between the modules and skipped; anything
else, module_layout included, must be exported by the kernel with the same
CRC. Counts are per unique symbol and per (module, symbol) entry. Exit status
0 means every module would load as far as MODVERSIONS is concerned.

Reference values for the stock audio modules: the stock Wear OS kernel and
the `sound` branch give 493 match, 0 mismatch, 0 missing (395 symbols
resolved between the modules, 1482 entries); the AsteroidOS 2.1 release
kernel gives 148 match, 343 mismatch, 2 missing.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bootimg  # noqa: E402
import verify  # noqa: E402

ZIMAGE_MAGIC = b"\x18\x28\x6f\x01"  # at offset 0x24 of an ARM zImage


def kernel_image(path):
    if os.path.isdir(path):
        path = os.path.join(path, "zImage-dtb")
    data = open(path, "rb").read()
    if data[:8] == b"ANDROID!":
        data = bootimg.payloads(data, bootimg.parse_header(data))[0]
    if data[0x24:0x28] == ZIMAGE_MAGIC:
        return verify.decompress_zimage(bootimg.split_kernel(data)[0])
    return data


def elf32_sections(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("not a 32-bit little-endian ELF file")
    shoff = struct.unpack_from("<I", data, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    secs = []
    for i in range(shnum):
        name, typ, _, _, off, size, link, _, _, _ = struct.unpack_from("<10I", data, shoff + i * shentsize)
        secs.append({"name": name, "type": typ, "off": off, "size": size, "link": link})
    strtab = secs[shstrndx]["off"]
    for s in secs:
        s["name"] = data[strtab + s["name"]:data.index(b"\0", strtab + s["name"])].decode()
    return secs


def module_info(path):
    """Return ({symbol: crc} from __versions, {symbols the module exports})."""
    data = open(path, "rb").read()
    secs = elf32_sections(data)
    versions, exports = {}, set()
    for s in secs:
        if s["name"] == "__versions":  # struct modversion_info: u32 crc + char name[60]
            for off in range(s["off"], s["off"] + s["size"], 64):
                crc = struct.unpack_from("<I", data, off)[0]
                versions[data[off + 4:off + 64].split(b"\0")[0].decode()] = crc
        elif s["type"] == 2:  # SHT_SYMTAB: exported symbols have a __ksymtab_<name> symbol
            strs = secs[s["link"]]["off"]
            for off in range(s["off"], s["off"] + s["size"], 16):
                name_off = struct.unpack_from("<I", data, off)[0]
                name = data[strs + name_off:data.index(b"\0", strs + name_off)].decode()
                if name.startswith("__ksymtab_"):
                    exports.add(name[len("__ksymtab_"):])
    return versions, exports


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kernel")
    ap.add_argument("modules")
    ap.add_argument("--all", action="store_true", help="list every failing entry, not just the first 30")
    a = ap.parse_args()

    kcrc = verify.export_crcs(kernel_image(a.kernel))
    if not kcrc:
        sys.exit("could not find the export tables in %s" % a.kernel)
    mods = {fn: module_info(os.path.join(a.modules, fn))
            for fn in sorted(os.listdir(a.modules)) if fn.endswith(".ko")}
    if not mods:
        sys.exit("no .ko files in %s" % a.modules)
    between = set().union(*(exports for _, exports in mods.values()))
    print("kernel:  %s, %d exported symbols with CRCs" % (a.kernel, len(kcrc)))
    print("modules: %d in %s, exporting %d symbols between them" % (len(mods), a.modules, len(between)))

    per_symbol, entries, bad = {}, {"match": 0, "mismatch": 0, "missing": 0, "module": 0}, []
    for fn, (versions, _) in mods.items():
        for sym, crc in sorted(versions.items()):
            if sym in between and sym != "module_layout":
                result = "module"
            elif sym not in kcrc:
                result = "missing"
            else:
                result = "match" if kcrc[sym] == crc else "mismatch"
            entries[result] += 1
            if per_symbol.get(sym) not in ("mismatch", "missing"):
                per_symbol[sym] = result
            if result in ("mismatch", "missing"):
                bad.append((fn, sym, result))
    count = lambda r: sum(1 for v in per_symbol.values() if v == r)
    print("symbols: %d match, %d mismatch, %d missing from the kernel, %d resolved between the modules"
          % (count("match"), count("mismatch"), count("missing"), count("module")))
    print("entries: %(match)d match, %(mismatch)d mismatch, %(missing)d missing, %(module)d between modules" % entries)
    for fn, sym, result in bad[:None if a.all else 30]:
        print("  %-28s %-40s %s" % (fn, sym, result))
    if len(bad) > 30 and not a.all:
        print("  ... %d more (--all lists them)" % (len(bad) - 30))
    print("RESULT: %s" % ("every module CRC matches the kernel" if not bad else "%d entries fail" % len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
