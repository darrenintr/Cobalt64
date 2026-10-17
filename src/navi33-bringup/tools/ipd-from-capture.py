#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build an IP Discovery binary from tools/capture-linux-ipd.sh output.

The result holds only the IP records in the capture (no GC info, harvest
or other tables), in the v1 binary header / v3 IPDS layout that
Navi33Discovery.h parses. Pass it to `n33compute --rom` on macOS.

Usage: tools/ipd-from-capture.py tests/fixtures/rx7600-linux-ipd.txt out.bin
"""
import struct
import sys

HWIDS = {"GC": 11, "MP0": 255, "MP1": 1, "SDMA0": 42, "SDMA1": 43,
         "DMU": 271, "MMHUB": 34, "NBIF": 108, "HDP": 41, "OSSSYS": 40,
         "ATHUB": 35}


def records(path):
    out = []
    for line in open(path, encoding="ascii"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, inst, hwid, ver, *bases = line.split()
        if HWIDS.get(name) != int(hwid):
            raise SystemExit(f"unexpected hw_id for {name}: {hwid}")
        major, minor, rev = (int(x) for x in ver.split("."))
        out.append((int(hwid), int(inst), major, minor, rev,
                    [int(b, 16) for b in bases]))
    return out


def build(recs):
    blob = bytearray(212)
    for hwid, inst, major, minor, rev, bases in recs:
        blob += struct.pack("<HBBBBBB", hwid, inst, len(bases),
                            major, minor, rev, 0)
        blob += b"".join(struct.pack("<I", b) for b in bases)
    end = len(blob)
    struct.pack_into("<IH", blob, 0, 0x28211407, 1)
    struct.pack_into("<HHHH", blob, 10, end, 128, 0, end - 128)
    struct.pack_into("<IHHIH", blob, 128, 0x53445049, 3, end - 128, 0, 1)
    struct.pack_into("<H", blob, 144, 208)
    struct.pack_into("<HH", blob, 208, 0, len(recs))
    struct.pack_into("<H", blob, 14, sum(blob[128:end]) & 0xffff)
    struct.pack_into("<H", blob, 8, sum(blob[10:end]) & 0xffff)
    return bytes(blob)


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__)
    data = build(records(argv[1]))
    with open(argv[2], "wb") as f:
        f.write(data)
    print(f"wrote {len(data)} bytes to {argv[2]}")


if __name__ == "__main__":
    main(sys.argv)
