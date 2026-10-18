#!/usr/bin/env python3
"""Read the PSP runtime database, or decode a saved copy. No GPU writes.

Capture on a NORMAL amdgpu Linux boot after full power-off:
  sudo python3 -I tools/scpm-read.py --capture
Decode offline:
  python3 -I tools/scpm-read.py --input runtime-db.bin

Only maps the 64-KiB database read-only when it is already inside BAR0.
Never changes the BAR aperture, sends commands, or accesses the ROM.
Source: torvalds/linux 58161ead29e50d70a317ea26b472e0c7508e69da:
  amdgpu/amdgpu_psp.{c,h}: database layout, offset, SCPM status.
  amdgpu/gmc_v11_0.c: mc_vram_size == real_vram_size at initialization.
  amdgpu/amdgpu_vram_mgr.c: mem_info_vram_total reports real_vram_size.
All paths above are under drivers/gpu/drm/amd/.
"""
import argparse
import json
import mmap
import os
from pathlib import Path
import struct
import time

BDF = "0000:09:00.0"
DB_SIZE = 0x10000
DB_OFFSET = 0x100000
COOKIE = 0x0ED5
VERSION = 0x0100
MAX_ENTRIES = 0x40
SCPM_MEANINGS = {
    0: "disabled",
    1: "enabled",
    2: "enabled with error",
}


def parse_runtime_db(data):
    if len(data) != DB_SIZE:
        raise ValueError("expected exactly 64 KiB of runtime database bytes")
    cookie, version, count = struct.unpack_from("<HHH", data)
    result = {"present": cookie == COOKIE, "cookie": cookie,
              "version": version, "scpm_status": None,
              "scpm_status_hex": None, "scpm_meaning": None,
              "scpm_enabled_by_amdgpu": None, "entries": []}
    if cookie != COOKIE:
        return result  # Absence is not evidence that the hardware permits masks.
    if version != VERSION:
        raise ValueError(f"unsupported runtime database version {version:#x}")
    if count >= MAX_ENTRIES:
        raise ValueError("invalid runtime database entry count")
    # C directory has u16 count + two padding bytes before u32 entry_type.
    directory_end = 8 + count * 8
    ranges = []
    seen = set()
    for i in range(count):
        kind, offset, size = struct.unpack_from("<IHH", data, 8 + i * 8)
        if not size or offset < directory_end or offset + size > DB_SIZE:
            raise ValueError("entry is empty, out of bounds, or overlaps directory")
        if any(offset < end and start < offset + size for start, end in ranges):
            raise ValueError("runtime database entries overlap")
        ranges.append((offset, offset + size))
        result["entries"].append({"type": kind, "offset": offset, "size": size})
        if kind not in (5, 6):
            continue
        if kind in seen:
            raise ValueError("duplicate boot-config or SCPM entry")
        seen.add(kind)
        if size < (8 if kind == 5 else 4):
            raise ValueError("boot-config or SCPM entry too short")
        value = struct.unpack_from("<I", data, offset)[0]
        if kind == 5:
            result["boot_config"] = value
        else:
            result["scpm_status"] = value
            result["scpm_status_hex"] = f"{value:#010x}"
            result["scpm_meaning"] = SCPM_MEANINGS.get(value, "unknown")
            # Upstream amdgpu tests scpm_status != SCPM_DISABLE, so an
            # undocumented nonzero value still activates its SCPM policy.
            result["scpm_enabled_by_amdgpu"] = value != 0
    return result


def capture():
    dev = Path("/sys/bus/pci/devices") / BDF
    if (dev / "vendor").read_text().strip() != "0x1002" or \
       (dev / "device").read_text().strip() != "0x7480":
        raise ValueError("not the RX 7600")
    if (dev / "driver").resolve().name != "amdgpu":
        raise ValueError("capture requires a normal amdgpu boot after full power-off")
    total = int((dev / "mem_info_vram_total").read_text())
    if total < DB_OFFSET or total > 8 * 1024**3 or total % 4096:
        raise ValueError("unexpected driver-reported VRAM size")
    start, end, flags = [int(x, 16) for x in (dev / "resource").read_text().splitlines()[0].split()]
    offset = total - DB_OFFSET
    if not start or not flags & 0x200 or offset + DB_SIZE > end - start + 1:
        raise ValueError("runtime database is outside BAR0; refusing aperture changes")
    fd = os.open(dev / "resource0", os.O_RDONLY | os.O_SYNC)
    try:
        with mmap.mmap(fd, DB_SIZE, mmap.MAP_SHARED, mmap.PROT_READ, offset=offset) as region:
            first = region[:]
            before = parse_runtime_db(first)
            time.sleep(0.1)
            second = region[:]
            after = parse_runtime_db(second)
    finally:
        os.close(fd)
    # Other runtime payloads may change; require stable directory and policy.
    if before != after:
        raise ValueError("runtime database metadata or SCPM status changed between reads")
    before.update({"bdf": BDF, "driver": "amdgpu", "vram_bytes": total,
                   "bar0_offset": offset, "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip()})
    return before, first, second


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--input", type=Path)
    mode.add_argument("--capture", action="store_true")
    ap.add_argument("--out", type=Path, help="new output directory for capture")
    args = ap.parse_args()
    try:
        if args.input:
            if args.out:
                ap.error("--out requires --capture")
            result = parse_runtime_db(args.input.read_bytes())
        else:
            result, first, second = capture()
            owner_home = Path(os.path.expanduser("~" + os.environ.get("SUDO_USER", "")))
            out = args.out or owner_home / "macos tahoe/navi33/host" / time.strftime("scpm-%Y%m%d-%H%M%S")
            out.mkdir(parents=True, exist_ok=False)
            (out / "runtime-db.bin").write_bytes(first)
            (out / "runtime-db-second.bin").write_bytes(second)
            (out / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
            print(f"Saved to {out}")
        print(json.dumps(result, indent=2))
        if result["scpm_status"] is None:
            print("SCPM policy unknown: no valid SCPM entry; do not infer mask permission.")
        elif result["scpm_meaning"] == "unknown":
            print(
                f"Unknown SCPM status {result['scpm_status_hex']}; upstream "
                "amdgpu treats any nonzero status as enabled. Keep allowed-mask "
                "messages disabled pending raw-database review."
            )
    except (OSError, ValueError) as exc:
        ap.exit(1, f"scpm-read: {exc}\n")


if __name__ == "__main__":
    main()
