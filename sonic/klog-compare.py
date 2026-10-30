#!/usr/bin/env python3
"""Compare two kernel logs: which messages does one boot have that the other doesn't?

  klog-compare.py [--window SECONDS] NORMAL.txt TRIAL.txt

Both files are `journalctl -k -b <boot> -o short-monotonic` output. Only lines
with a kernel timestamp below --window (default 120 s, i.e. the boot itself)
are compared. Numbers, hex values, MAC addresses and SSIDs are normalised, the
kernel command line (which holds the serial number) is dropped, then the two
multisets of messages are diffed. Timing noise is expected; look for new
errors, warnings, missing probes and missing devices.
"""
import argparse
import collections
import re
import sys

LINE = re.compile(r"^\[\s*(\d+\.\d+)\]\s+\S+\s+([^:\s]+):\s?(.*)$")
NORMALISE = [
    (re.compile(r"(?i)\b[0-9a-f]{2}(:[0-9a-f]{2}){5}\b"), "<mac>"),
    (re.compile(r'(?i)(ssid[\s:=]*)("[^"]*"|\S+)'), r"\1<ssid>"),
    (re.compile(r"(?i)0x[0-9a-f]+"), "0x#"),
    (re.compile(r"(?i)\b[0-9a-f]{8,}\b"), "<hex>"),
    (re.compile(r"\d+"), "#"),
]
DROP = re.compile(r"Kernel command line:|androidboot\.serialno|serialno")


def load(path, window):
    msgs = collections.Counter()
    for raw in open(path, errors="replace"):
        m = LINE.match(raw.rstrip("\n"))
        if not m or float(m.group(1)) > window:
            continue
        text = m.group(3) if m.group(2) == "kernel" else m.group(2) + ": " + m.group(3)
        if DROP.search(text):
            continue
        for rx, rep in NORMALISE:
            text = rx.sub(rep, text)
        msgs[text.strip()] += 1
    return msgs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("normal")
    ap.add_argument("trial")
    ap.add_argument("--window", type=float, default=120.0)
    a = ap.parse_args()
    n, t = load(a.normal, a.window), load(a.trial, a.window)
    print("first %.0f s: normal boot %d messages (%d distinct), trial boot %d (%d distinct)"
          % (a.window, sum(n.values()), len(n), sum(t.values()), len(t)))
    only_t = sorted(set(t) - set(n))
    only_n = sorted(set(n) - set(t))
    print("\n--- only in the TRIAL boot (%d distinct):" % len(only_t))
    for s in only_t:
        print("  %3dx  %s" % (t[s], s))
    print("\n--- only in the NORMAL boot (%d distinct):" % len(only_n))
    for s in only_n:
        print("  %3dx  %s" % (n[s], s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
