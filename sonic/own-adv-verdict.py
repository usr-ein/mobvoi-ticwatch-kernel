#!/usr/bin/env python3
"""own-adv-check's verdict, from btmon's capture (fork PR #12, round 2's note 2).

own-adv-check tells only whether it caught the case: no link before and after
its update. Whether the case passed is the capture's to say, by this criterion:

  - the standard set's LE Set Advertising Enable (01), answered 0x00, comes
    before the vendor set's disable (Vendor 0xfd54: 05 00 01). An enable the
    chip accepts means no link: it refuses one while connected;
  - every vendor command is answered 0x00;
  - from that disable until the iPhone's LE Connection Complete, no command
    goes to the standard set (0x2005 to 0x200a), and no HCI Reset: a power
    cycle starts the standard set again, so steps 5 and 6, which step 7's
    power off follows, never count;
  - 0x55 for handle 0 follows that connection: the standard set, ended by it.

  sonic/own-adv-verdict.py CAPTURE.btsnoop
  sonic/own-adv-verdict.py --self-test

Prints each disable the capture holds and what became of it, then one of
PASS (a disable meets every point), NOT CAPTURED (none was made with the
standard set alone on air), INCONCLUSIVE (one was, but the capture can't
settle it), or FAIL (a vendor command refused, or never answered). Reads
btmon's own format (btsnoop, datalink 2001). Prints no address. --self-test
reads synthetic captures with a known verdict each.
"""

import os
import struct
import sys
import tempfile
from datetime import UTC, datetime

BTSNOOP_EPOCH_DELTA = 0x00DCDDB30F2F8000  # µs from year 0 to 1970
COMMAND, EVENT = 2, 3
VENDOR_MULTI_ADV = 0xFD54
LE_SET_ADV_ENABLE = 0x200A
STANDARD_SET = range(0x2005, 0x200B)  # random address .. advertise enable
HCI_RESET = 0x0C03


def packets(path):
    """(time, opcode, data) for each command and event of the capture."""
    with open(path, "rb") as f:
        head = f.read(16)
        if head[:8] != b"btsnoop\0" or struct.unpack(">I", head[12:16])[0] != 2001:
            sys.exit(f"{path}: not btmon's btsnoop (datalink 2001)")
        while rec := f.read(24):
            _, included, flags, _, stamp = struct.unpack(">IIIIq", rec)
            data = f.read(included)
            opcode = flags & 0xFFFF
            if opcode in (COMMAND, EVENT):
                when = datetime.fromtimestamp((stamp - BTSNOOP_EPOCH_DELTA) / 1e6, UTC)
                yield when, opcode, data


def verdict(path):
    """The capture's disables, and the verdict."""
    cmds = []  # (time, opcode, params), in order
    answers = {}  # command index -> its status
    events = []  # (time, kind, data)
    pending = []  # command indexes waiting for their answer, oldest first
    for when, kind, data in packets(path):
        if kind == COMMAND:
            op = data[0] | data[1] << 8
            cmds.append((when, op, data[3:]))
            pending.append(len(cmds) - 1)
            events.append((when, "cmd", len(cmds) - 1))
            continue
        code = data[0]
        if code in (0x0E, 0x0F):  # Command Complete, Command Status
            op = data[3] | data[4] << 8 if code == 0x0E else data[4] | data[5] << 8
            status = data[5] if code == 0x0E else data[2]
            for i in pending:
                if cmds[i][1] == op:
                    answers[i] = status
                    pending.remove(i)
                    break
        elif code == 0x3E and data[2] in (0x01, 0x0A) and data[3] == 0:
            events.append((when, "connected", 0))  # LE (Enhanced) Connection Complete
        elif code == 0xFF and len(data) > 3 and data[2] == 0x55:
            events.append((when, "0x55", data[3]))  # the set it reports
    # Every vendor command answered 0x00: one never answered fails too.
    vendor = [i for i, c in enumerate(cmds) if c[1] == VENDOR_MULTI_ADV]
    refused = [i for i in vendor if i in answers and answers[i] != 0]
    unanswered = [i for i in vendor if i not in answers]
    if refused or unanswered:
        return [], (
            f"FAIL: of {len(vendor)} vendor commands, "
            f"{len(refused)} refused, {len(unanswered)} never answered"
        )

    lines, passed = [], False
    standard_on = False
    for n, (when, kind, val) in enumerate(events):
        if kind == "connected":
            standard_on = False
            continue
        if kind != "cmd":
            continue
        _, op, params = cmds[val]
        if op == LE_SET_ADV_ENABLE and answers.get(val) == 0:
            standard_on = params[:1] == b"\x01"
        elif op == HCI_RESET:
            standard_on = False
        if op != VENDOR_MULTI_ADV or bytes(params[:3]) != b"\x05\x00\x01":
            continue
        clock = when.astimezone().strftime("%H:%M:%S.%f")
        if not standard_on:
            lines.append(
                f"{clock}  disable, the standard set not on air alone: not the case"
            )
            continue
        outcome = "no reconnection in the capture: inconclusive"
        for when2, kind2, val2 in events[n + 1 :]:
            if kind2 == "cmd" and (
                cmds[val2][1] in STANDARD_SET or cmds[val2][1] == HCI_RESET
            ):
                outcome = f"spoiled: {cmds[val2][1]:#06x} to the standard set before any reconnection"
                break
            if kind2 == "connected":
                after = [
                    w for w, k, v in events if k == "0x55" and v == 0 and w >= when2
                ]
                if after and (after[0] - when2).total_seconds() < 2:
                    outcome = (
                        "PASS: the standard set alone on air until the iPhone came back"
                    )
                    passed = True
                else:
                    outcome = (
                        "no 0x55 for handle 0 after the reconnection: inconclusive"
                    )
                break
        lines.append(f"{clock}  disable with the standard set alone on air: {outcome}")
    if passed:
        return lines, "PASS"
    if any("alone on air:" in line for line in lines):
        return lines, "INCONCLUSIVE"
    return lines, "NOT CAPTURED"


def synthetic(path, steps):
    """A btmon capture of `steps`: ("cmd", opcode, params), ("cc", opcode, status),
    ("connected",) or ("0x55", set), 10 ms apart."""
    with open(path, "wb") as f:
        f.write(b"btsnoop\0" + struct.pack(">II", 1, 2001))
        stamp = BTSNOOP_EPOCH_DELTA + 1_794_000_000_000_000
        for step in steps:
            if step[0] == "cmd":
                kind, data = (
                    COMMAND,
                    struct.pack("<HB", step[1], len(step[2])) + step[2],
                )
            elif step[0] == "cc":
                kind, data = (
                    EVENT,
                    bytes([0x0E, 4, 1]) + struct.pack("<HB", step[1], step[2]),
                )
            elif step[0] == "connected":
                kind, data = EVENT, bytes([0x3E, 19, 0x01, 0x00]) + bytes(17)
            else:
                kind, data = EVENT, bytes([0xFF, 5, 0x55, step[1], 0x00, 0x40, 0x00])
            f.write(struct.pack(">IIIIq", len(data), len(data), kind, 0, stamp) + data)
            stamp += 10_000


def self_test():
    """Synthetic captures, each with the verdict it must get."""
    enable = [("cmd", LE_SET_ADV_ENABLE, b"\x01"), ("cc", LE_SET_ADV_ENABLE, 0)]
    disable = ("cmd", VENDOR_MULTI_ADV, b"\x05\x00\x01")
    back = [("connected",), ("0x55", 0)]
    cases = {
        "the case, passed": (
            enable + [disable, ("cc", VENDOR_MULTI_ADV, 0)] + back,
            "PASS",
        ),
        "the disable refused": (
            enable + [disable, ("cc", VENDOR_MULTI_ADV, 0x0C)] + back,
            "FAIL",
        ),
        "the disable never answered": (enable + [disable] + back, "FAIL"),
        "the standard set started again": (
            enable + [disable, ("cc", VENDOR_MULTI_ADV, 0)] + enable + back,
            "INCONCLUSIVE",
        ),
        "a link up at the disable": (
            enable + [("connected",), disable, ("cc", VENDOR_MULTI_ADV, 0)],
            "NOT CAPTURED",
        ),
    }
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, (steps, want) in cases.items():
            path = os.path.join(tmp, "capture.btsnoop")
            synthetic(path, steps)
            got = verdict(path)[1]
            ok = got.split(":")[0] == want
            failed += not ok
            print(f"{'ok  ' if ok else 'FAIL'} {name}: {got}")
    sys.exit(1 if failed else 0)


def main():
    if sys.argv[1:] == ["--self-test"]:
        self_test()
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    lines, result = verdict(sys.argv[1])
    for line in lines:
        print(line)
    print(f"RESULT: {result}")
    sys.exit(0 if result == "PASS" else 1)


if __name__ == "__main__":
    main()
