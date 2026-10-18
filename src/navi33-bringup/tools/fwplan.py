#!/usr/bin/env python3
"""Load plan for the RX 7600 (GC 11.0.2) IP firmware, from the real files.

Read-only and Linux-side: it reads /lib/firmware/amdgpu/*.bin.zst, applies the
rules Linux uses to turn each image into PSP LOAD_IP_FW records
(amdgpu_ucode_init_single_fw, amdgpu_gfx_cp_init_microcode, amdgpu_rlc.c,
amdgpu_sdma.c, amdgpu_mes.c, imu_v11_0.c; psp_load_non_psp_fw for the order),
checks every offset against the file size, and prints the list with SHA-256
pins. It never touches the card. The SMU image is excluded on purpose (user's
decision, 2026-10-18): Linux sends it first, but this card's SMU is already
running from the VBIOS and re-sending it is a bigger commitment.

  python3 -I tools/fwplan.py [--dir /lib/firmware/amdgpu] [--json]
"""
import hashlib, json, struct, subprocess, sys

# psp_gfx_if.h enum psp_gfx_fw_type (numbers read from the upstream header)
T = dict(SDMA_UCODE_TH0=71, SDMA_UCODE_TH1=72, RS64_PFP=87, RS64_ME=88,
         RS64_MEC=89, RS64_PFP_P0_STACK=90, RS64_PFP_P1_STACK=91,
         RS64_ME_P0_STACK=92, RS64_ME_P1_STACK=93, RS64_MEC_P0_STACK=94,
         RS64_MEC_P1_STACK=95, RS64_MEC_P2_STACK=96, RS64_MEC_P3_STACK=97,
         CP_MES=33, MES_STACK=34, CP_MES_KIQ=81, MES_KIQ_STACK=82, IMU_I=68,
         IMU_D=69, RLC_RESTORE_LIST_SRM_CNTL=22, RLC_RESTORE_LIST_GPM_MEM=20,
         RLC_RESTORE_LIST_SRM_MEM=21, RLC_IRAM=26, RLC_DRAM_BOOT=48, RLC_P=25,
         RLC_V=7, RLC_G=8)
FILES = dict(pfp="gc_11_0_2_pfp", me="gc_11_0_2_me", mec="gc_11_0_2_mec",
             rlc="gc_11_0_2_rlc", imu="gc_11_0_2_imu", sdma="sdma_6_0_2",
             mes="gc_11_0_2_mes_2", mes1="gc_11_0_2_mes1")
PAGE = 4096


class Fw:
    def __init__(self, name, data):
        self.name, self.d = name, data
        (self.size, self.hdr_sz, self.maj, self.min, self.ipmaj, self.ipmin,
         self.ver, self.usz, self.uoff, self.crc) = struct.unpack_from("<IIHHHHIIII", data, 0)
        if self.size != len(data):
            raise SystemExit(f"{name}: header size {self.size} != file size {len(data)}")
        self.sha = hashlib.sha256(data).hexdigest()

    def u32(self, off):
        if off + 4 > len(self.d):
            raise SystemExit(f"{self.name}: header read past the end at {off}")
        return struct.unpack_from("<I", self.d, off)[0]

    def blob(self, off, size):
        if off < self.hdr_sz or size <= 0 or off + size > len(self.d):
            raise SystemExit(f"{self.name}: image {off}+{size} lies outside the file")
        return self.d[off:off + size]


def load(dirpath, base):
    p = f"{dirpath}/{base}.bin.zst"
    return Fw(base, subprocess.run(["zstd", "-q", "-d", "-c", p], check=True,
                                   capture_output=True).stdout)


def plan(dirpath):
    f = {k: load(dirpath, v) for k, v in FILES.items()}
    out = []   # (enum order, name, type, source file, offset, size, data)

    def add(order, name, typ, fw, off, size):
        out.append(dict(order=order, name=name, type=typ, file=fw.name, offset=off,
                        size=size, sha256=hashlib.sha256(fw.blob(off, size)).hexdigest()))

    # SDMA 6.0.2: header v2.0, context thread (TH0) then control thread (TH1)
    s = f["sdma"]
    assert (s.maj, s.min) == (2, 0)
    # sdma_firmware_header_v2_0: ctx_ucode_size @36, ctl_ucode_offset @48, ctl size @52
    add(9, "SDMA_UCODE_TH0", T["SDMA_UCODE_TH0"], s, s.uoff, s.u32(36))
    add(10, "SDMA_UCODE_TH1", T["SDMA_UCODE_TH1"], s, s.u32(48), s.u32(52))
    # CP: gfx_firmware_header_v2_0 (RS64). Stacks are the "data" part, sent twice
    # (P0 and P1; MEC has P0..P3), each with data_size_bytes.
    for key, base, stacks in (("pfp", 15, ("PFP_P0", "PFP_P1")),
                              ("me", 16, ("ME_P0", "ME_P1")),
                              ("mec", 17, ("MEC_P0", "MEC_P1", "MEC_P2", "MEC_P3"))):
        c = f[key]
        assert (c.maj, c.min) == (2, 0), f"{c.name}: expected gfx header v2.0"
        # gfx_firmware_header_v2_0 after the 32-byte common header: feature
        # version, ucode size/offset, data size/offset
        feature, usz, uoff, dsz, doff = (c.u32(32 + 4 * i) for i in range(5))
        name = key.upper()
        add(base, f"CP_RS64_{name}", T[f"RS64_{name}"], c, uoff, usz)
        stack_ids = {"PFP": (18, 19), "ME": (20, 21), "MEC": (22, 23, 24, 25)}[name]
        for sid, st in zip(stack_ids, stacks):
            add(sid, f"CP_RS64_{st}_STACK", T[f"RS64_{st}_STACK"], c, doff, dsz)
    # MES: scheduler pipe from gc_11_0_2_mes_2.bin, KIQ pipe from gc_11_0_2_mes1.bin
    for key, uid, did, tu, td in (("mes", 30, 31, "CP_MES", "MES_STACK"),
                                  ("mes1", 32, 33, "CP_MES_KIQ", "MES_KIQ_STACK")):
        m = f[key]
        assert (m.maj, m.min) == (1, 0)
        add(uid, "CP_" + key.upper(), T[tu], m, m.u32(40), m.u32(36))
        add(did, "CP_" + key.upper() + "_DATA", T[td], m, m.u32(52), m.u32(48))
    # IMU: IRAM then DRAM, both inside the ucode array
    i = f["imu"]
    iram, dram = i.u32(32 + 0), i.u32(32 + 8)
    add(34, "IMU_I", T["IMU_I"], i, i.uoff, iram)
    add(35, "IMU_D", T["IMU_D"], i, i.uoff + iram, dram)
    # RLC v2.3: restore lists (v2.1), IRAM/DRAM (v2.2), RLC_P/RLC_V (v2.3), RLC_G
    r = f["rlc"]
    assert (r.maj, r.min) == (2, 3)
    # rlc_firmware_header_v2_0 is the 32-byte common header plus 18 dwords (104 B).
    # v2_1 then has direct_reg_list_length and {ucode_ver, feature_ver, size,
    # offset} for CNTL, GPM, SRM; v2_2 IRAM/DRAM {size, offset}; v2_3 RLC_P and
    # RLC_V {ver, feature_ver, size, offset}. Offsets are from the file start.
    base21 = 32 + 18 * 4 + 4
    for n, (nm, idn, typ) in enumerate((("CNTL", 41, "RLC_RESTORE_LIST_SRM_CNTL"),
                                        ("GPM_MEM", 42, "RLC_RESTORE_LIST_GPM_MEM"),
                                        ("SRM_MEM", 43, "RLC_RESTORE_LIST_SRM_MEM"))):
        size, off = r.u32(base21 + 16 * n + 8), r.u32(base21 + 16 * n + 12)
        if size:
            add(idn, "RLC_RESTORE_LIST_" + nm, T[typ], r, off, size)
    v22 = base21 + 16 * 3
    add(44, "RLC_IRAM", T["RLC_IRAM"], r, r.u32(v22 + 4), r.u32(v22 + 0))
    add(45, "RLC_DRAM", T["RLC_DRAM_BOOT"], r, r.u32(v22 + 12), r.u32(v22 + 8))
    v23 = v22 + 16
    add(48, "RLC_P", T["RLC_P"], r, r.u32(v23 + 12), r.u32(v23 + 8))
    add(49, "RLC_V", T["RLC_V"], r, r.u32(v23 + 28), r.u32(v23 + 24))
    add(50, "RLC_G", T["RLC_G"], r, r.uoff, r.usz)
    out.sort(key=lambda e: e["order"])
    return f, out


def main():
    d = "/lib/firmware/amdgpu"
    if "--dir" in sys.argv:
        d = sys.argv[sys.argv.index("--dir") + 1]
    files, items = plan(d)
    if "--tsv" in sys.argv:
        # one line per file ("F key name bytes sha256"), then one per record
        # ("R order name type key offset size sha256"); read by the C++ test
        for k, v in files.items():
            print("F", k, v.name, v.size, v.sha)
        for e in items:
            key = next(k for k, v in files.items() if v.name == e["file"])
            print("R", e["order"], e["name"], e["type"], key, e["offset"], e["size"], e["sha256"])
        return
    if "--json" in sys.argv:
        print(json.dumps(dict(files={k: dict(name=v.name, bytes=v.size, sha256=v.sha,
                                             header=f"{v.maj}.{v.min}", version=hex(v.ver))
                                     for k, v in files.items()}, items=items), indent=1))
        return
    print("files (decompressed):")
    for k, v in files.items():
        print(f"  {v.name:18} {v.size:7} B  header {v.maj}.{v.min}  ucode {v.ver:#x}  sha256 {v.sha[:16]}…")
    print("\nPSP LOAD_IP_FW records, in Linux's order (SMU left out):")
    tot = 0
    for e in items:
        pad = (e["size"] + PAGE - 1) // PAGE * PAGE
        tot += pad
        print(f"  {e['order']:2} {e['name']:22} type {e['type']:3}  {e['file']:16} @{e['offset']:7} {e['size']:7} B")
    print(f"\n{len(items)} records, then GFX_CMD_ID_AUTOLOAD_RLC after RLC_G.")
    print(f"staging total, each record page-aligned: {tot} B = {tot / 1048576:.2f} MiB")


if __name__ == "__main__":
    main()
