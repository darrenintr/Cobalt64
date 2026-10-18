#!/usr/bin/env python3
"""Reference dump of the RX 7600's key registers while Linux's amdgpu drives it.

Run as root on a NORMAL Ubuntu boot (amdgpu loaded, after a FULL power-off, so
the PSP started clean). It only reads: BAR5 through a read-only mmap of
/sys/bus/pci/devices/<bdf>/resource5, plus amdgpu's debugfs info files. The one
thing it changes is the driver's own GFXOFF debug switch: reading graphics
registers while the engine sits in GFXOFF can hang the machine, so it disables
GFXOFF through /sys/kernel/debug/dri/<n>/amdgpu_gfxoff for the reads and
restores it. It never touches the VBIOS (hard rule 5) and writes nothing else.

  sudo python3 -I tools/ref-dump.py [OUT_FILE]

The register list and offsets are the same ones n33peek reads in the guest, so
the output can be compared line by line with results-*-guest.txt:
BAR5 dword = base + register, bases from the card's IP Discovery
(GC seg0 0x1260, GC seg1 0xA000, MP0/MP1 seg0 0x16000).
"""
import glob, mmap, os, re, struct, subprocess, sys, time

BDF = "0000:09:00.0"
OUT = sys.argv[1] if len(sys.argv) > 1 else time.strftime(
    os.path.expanduser("~/macos tahoe/navi33/host/reference-%Y%m%d-%H%M%S.txt"))

GC0, GC1, MP = 0x1260, 0xA000, 0x16000
# (name, base, first register, dword count): the same windows as n33peek.
PSP_SMU = [("PSP_C2PMSG", MP, 0x60, 96), ("SMU_C2PMSG", MP, 0x260, 72)]
GC = [
    ("IMU_SCRATCH", GC1, 0x4068, 16), ("IMU_CORE_STATUS", GC1, 0x40b7, 1),
    ("IMU_GFX_RESET_CTRL", GC1, 0x40bc, 1), ("IMU_RLC_STATUS", GC1, 0x4054, 1),
    ("RLC_CNTL", GC1, 0x4c00, 1), ("RLC_STAT", GC1, 0x4c04, 1),
    ("RLC_GPM_STAT", GC1, 0x4e6b, 1), ("RLC_BOOTLOAD_STATUS", GC1, 0x4e82, 1),
    ("GRBM_STATUS2", GC0, 0x0da2, 1), ("GRBM_STATUS", GC0, 0x0da4, 4),
    ("CP_STAT", GC0, 0x0f3f, 2), ("CP_CPC_STATUS", GC0, 0x0e24, 1),
    ("CP_CPF_STATUS", GC0, 0x0e27, 1), ("CP_MEC_ME_CNTL", GC1, 0x0802, 2),
    ("CP_MEC_RS64_CNTL", GC1, 0x2904, 1),
    ("SDMA0_STATUS", GC0, 0x0025, 5), ("SDMA0_ID", GC0, 0x0034, 2),
    ("SDMA0_STATUS2", GC0, 0x0038, 1), ("SDMA0_Q0_CTL", GC0, 0x0088, 11),
    ("GCVM_L2_CNTL", GC0, 0x15bc, 1), ("GCMC_FB_LOCATION", GC0, 0x1678, 2),
]
FIXED = [("MMMC_FB_LOCATION", 0x1A000, 0x086c, 2), ("NBIO_MEMSIZE", 0x0D20, 0x00c3, 1)]


def main():
    if os.geteuid() != 0:
        sys.exit("run as root")
    drv = os.path.basename(os.path.realpath(f"/sys/bus/pci/devices/{BDF}/driver"))
    lines = [f"# reference dump {time.strftime('%F %T')}  driver={drv}  host={os.uname().release}"]
    if drv != "amdgpu":
        lines.append("# WARNING: amdgpu is not bound; this is not a reference of a Linux-driven card")
    fd = os.open(f"/sys/bus/pci/devices/{BDF}/resource5", os.O_RDONLY | os.O_SYNC)
    bar = mmap.mmap(fd, 1 << 20, mmap.MAP_SHARED, mmap.PROT_READ)

    def rd(base, reg):
        return struct.unpack_from("<I", bar, (base + reg) * 4)[0]

    def dump(title, items):
        lines.append(f"\n## {title}")
        for name, base, first, count in items:
            vals = [rd(base, first + i) for i in range(count)]
            lines.append(f"== {name}  reg {first:#06x} x{count}")
            for i in range(0, count, 8):
                lines.append(f"  {first + i:#06x}: " + " ".join(f"{v:08x}" for v in vals[i:i + 8]))

    dump("PSP and SMU mailboxes (not gated by GFXOFF)", PSP_SMU + FIXED)
    # GC registers: disable GFXOFF first, restore after.
    gfxoff = glob.glob("/sys/kernel/debug/dri/*/amdgpu_gfxoff")
    prev = None
    try:
        if drv == "amdgpu" and gfxoff:
            prev = open(gfxoff[0]).read().strip()
            lines.append(f"\n# amdgpu_gfxoff was: {prev!r}; disabling it for the GC reads")
            with open(gfxoff[0], "w") as f:
                f.write("0")
            time.sleep(0.5)
        elif drv == "amdgpu":
            lines.append("\n# no amdgpu_gfxoff debugfs file: skipping the GC reads (could hang)")
        if drv != "amdgpu" or gfxoff:
            dump("Graphics engine (GFXOFF disabled while reading)", GC)
    finally:
        if prev is not None:
            with open(gfxoff[0], "w") as f:
                f.write("1" if prev in ("1", "enabled", "Y") else "0")
            lines.append(f"# amdgpu_gfxoff restored to {prev!r}")
    # amdgpu's own account of what it loaded and what it runs.
    for name in ("amdgpu_firmware_info", "amdgpu_pm_info", "amdgpu_gpu_info"):
        for path in glob.glob(f"/sys/kernel/debug/dri/*/{name}"):
            lines.append(f"\n## {name}\n" + open(path, errors="replace").read())
    out = subprocess.run(["lspci", "-s", BDF, "-vv"], capture_output=True, text=True).stdout
    lines.append("\n## lspci\n" + out)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("\n".join(lines) + "\n")
    print(f"wrote {OUT}")
    print("\n".join(l for l in lines if l.startswith(("# ", "== IMU_GFX", "== RLC_BOOT")) or
                    re.match(r"\s+0x(4068|40bc|4e82)", l)))


if __name__ == "__main__":
    main()
