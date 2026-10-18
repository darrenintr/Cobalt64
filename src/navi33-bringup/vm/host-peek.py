#!/usr/bin/env python3
# Root, VM boot only, no VM running: read the PSP mailbox and the PSP plan's
# DCN registers straight from BAR5 on the host, read-only (PROT_READ), so the
# card can be checked before the guest's firmware touches it.
#   sudo python3 -I vm/host-peek.py [BDF]
# On 2026-10-18 the monitor lost its picture the moment the VM started, and
# every DCN register read all-ones inside the guest.
# Offsets: PSP C2PMSG_32..127 are MP0 dwords 0x60..0xbf (base 0x16000,
# C2PMSG_35 = 0x63, AGENTS.md); DCN from Navi33Dcn321.h kReadOnlyRegisters
# (DCN 3.2.1 seg2, base 0x34C0, from the RX 7600's IP Discovery).
import mmap, os, struct, sys, time

bdf = sys.argv[1] if len(sys.argv) > 1 else "0000:09:00.0"
path = f"/sys/bus/pci/devices/{bdf}/resource5"
fd = os.open(path, os.O_RDONLY | os.O_SYNC)
bar5 = mmap.mmap(fd, 1 << 20, mmap.MAP_SHARED, mmap.PROT_READ)

def rd(dword):
    return struct.unpack_from("<I", bar5, dword * 4)[0]

for n in (33, 35, 58, 81):
    print(f"MP0 C2PMSG_{n:<3} {rd(0x16000 + 0x60 + n - 32):#010x}")
DCN = 0x34C0
regs = [("DCN_VM_FB_LOCATION_BASE", 0x475), ("DCN_VM_FB_LOCATION_TOP", 0x476)]
pipes = [(0x607, 0x60a, 0x60b, 0x1b4c), (0x6e3, 0x6e6, 0x6e7, 0x1bcc),
         (0x7bf, 0x7c2, 0x7c3, 0x1c4c), (0x89b, 0x89e, 0x89f, 0x1ccc)]
for p, (pitch, lo, hi, fc) in enumerate(pipes):
    regs += [(f"HUBPREQ{p}_PITCH", pitch), (f"HUBPREQ{p}_SURFACE_ADDR", lo),
             (f"HUBPREQ{p}_SURFACE_ADDR_HI", hi), (f"OTG{p}_FRAME_COUNT", fc)]
for name, off in regs:
    print(f"{name:28} {rd(DCN + off):#010x}")
time.sleep(1)
for name, off in regs:
    if "FRAME" in name:
        print(f"{name:28} {rd(DCN + off):#010x} (1 s later)")
