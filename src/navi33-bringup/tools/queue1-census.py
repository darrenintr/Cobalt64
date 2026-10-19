#!/usr/bin/env python3
"""Decode the Queue1 preflight state from a results file's n33peek section.

Read-only and offline: it parses text, never opens a device. Bit meanings
come from Linux gc_11_0_0_sh_mask.h and mmhub_3_0_2_sh_mask.h; what Linux
sets before an SDMA ring runs comes from gmc_v11_0.c, gfxhub_v3_0.c,
mmhub_v3_0_2.c and sdma_v6_0.c. A "gap" is a bit Linux sets that reads
clear here. It says nothing about whether setting it would be enough.

usage: queue1-census.py RESULTS_FILE
"""
import re
import sys

HEADER = re.compile(r"^== (\w+)\s+\S+ seg(\d) reg 0x([0-9a-f]+) x(\d+)")
ROW = re.compile(r"^\s+0x([0-9a-f]+):((?:\s+[0-9a-f]{8})+)\s*$")


def parse(text):
    """Return {window: (first, [dword, ...])} for every n33peek window."""
    windows, name = {}, None
    for line in text.splitlines():
        m = HEADER.match(line)
        if m:
            name = m.group(1)
            windows[name] = (int(m.group(3), 16), [])
            continue
        m = ROW.match(line)
        if m and name:
            windows[name][1].extend(int(v, 16) for v in m.group(2).split())
        elif line.startswith("== ") or line.startswith("GPU writes"):
            name = None
    return windows


def word(w, window, index=0):
    if window not in w or index >= len(w[window][1]):
        return None
    return w[window][1][index]


def bit(value, shift):
    return None if value is None else (value >> shift) & 1


def field(value, shift, width):
    return None if value is None else (value >> shift) & ((1 << width) - 1)


def hub(w, prefix, l2, l1, ctx):
    l1v = word(w, l1)
    return {
        "l2_cntl": word(w, l2),
        "l2_cache_enabled": bit(word(w, l2), 0),        # ENABLE_L2_CACHE
        "l1_tlb_cntl": l1v,
        "l1_tlb_enabled": bit(l1v, 0),                   # ENABLE_L1_TLB
        "system_access_mode": field(l1v, 3, 2),          # Linux sets 3
        "advanced_driver_model": bit(l1v, 6),            # Linux sets 1
        "ctx0_cntl": word(w, ctx, 0),
        "ctx0_enabled": bit(word(w, ctx, 0), 0),
        "ctx1_cntl": word(w, ctx, 1),
        "ctx1_enabled": bit(word(w, ctx, 1), 0),
        "ctx1_depth": field(word(w, ctx, 1), 1, 2),
        "prefix": prefix,
    }


def census(w):
    psp81, psp58 = word(w, "PSP_C2PMSG", 49), word(w, "PSP_C2PMSG", 26)
    q1 = [word(w, "SDMA0_Q1_RB", i) for i in range(7)]
    f32 = word(w, "SDMA0_F32_CNTL")
    out = {
        "sos_alive": None if psp81 is None else psp81 != 0,
        "sos_version": psp58,
        "rlc_bootload_complete": bit(word(w, "RLC_BOOTLOAD_STATUS"), 31),
        "imu_gfx_reset_ctrl": word(w, "IMU_GFX_RESET_CTRL"),
        "sdma_id": word(w, "SDMA0_ID"),
        "sdma_f32_cntl": f32,
        "sdma_halted": bit(f32, 0),                      # SDMA0_F32_CNTL.HALT
        "q1_rb_cntl": q1[0],
        "q1_enabled": bit(q1[0], 0),                     # RB_ENABLE
        "q1_vmid": field(q1[0], 24, 4),                  # RB_VMID
        "q1_base": None if q1[1] is None else (q1[2] << 32) | q1[1],
        "q1_rptr": q1[3], "q1_wptr": q1[5],
        "q1_doorbell_enabled": bit(word(w, "SDMA0_Q1_CTL", 10), 28),
        "mmhub": hub(w, "MM", "MMVM_L2_CNTL", "MMMC_VM_MX_L1_TLB_CNTL",
                     "MMVM_CONTEXT_CNTL"),
        "gfxhub": hub(w, "GC", "GCVM_L2_CNTL", "GCMC_VM_MX_L1_TLB_CNTL",
                      "GCVM_CONTEXT_CNTL"),
    }
    gaps = []
    if out["sos_alive"] is False:
        gaps.append("PSP sOS not loaded")
    if out["rlc_bootload_complete"] == 0:
        gaps.append("RLC bootload not complete (GC not up)")
    # GC reads 0 while it is powered down (AGENTS.md hard rule 4), so GC
    # values count only once the RLC bootload has completed.
    gc_up = out["rlc_bootload_complete"] == 1
    if gc_up and out["sdma_halted"] == 1:
        gaps.append("SDMA0_F32_CNTL.HALT set (sdma_v6_0 clears it)")
    for name in ("gfxhub", "mmhub") if gc_up else ("mmhub",):
        h = out[name]
        if h["l2_cache_enabled"] == 0:
            gaps.append(f"{name} L2 cache off ({h['prefix']}VM_L2_CNTL.ENABLE_L2_CACHE)")
        if h["l1_tlb_enabled"] == 0:
            gaps.append(f"{name} L1 TLB off (MX_L1_TLB_CNTL.ENABLE_L1_TLB)")
        if h["ctx1_enabled"] == 0:
            gaps.append(f"{name} VMID1 context disabled")
    if gc_up and out["q1_enabled"] == 0:
        gaps.append("SDMA0 Queue1 ring disabled (expected before setup)")
    out["gc_values_meaningful"] = gc_up
    unread = [k for k in ("sdma_f32_cntl",) if out[k] is None]
    unread += [f"{n}.l1_tlb_cntl" for n in ("gfxhub", "mmhub")
               if out[n]["l1_tlb_cntl"] is None]
    out["gaps"], out["unread"] = gaps, unread
    return out


def fmt(v):
    if v is None:
        return "not read"
    if isinstance(v, bool):
        return "yes" if v else "no"
    return f"0x{v:x}" if isinstance(v, int) and v > 9 else str(v)


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    with open(argv[1], encoding="utf-8", errors="replace") as f:
        out = census(parse(f.read()))
    for k, v in out.items():
        if isinstance(v, dict):
            for hk, hv in v.items():
                if hk != "prefix":
                    print(f"{k}.{hk}: {fmt(hv)}")
        elif k not in ("gaps", "unread"):
            print(f"{k}: {fmt(v)}")
    print("not read (older kext): " + (", ".join(out["unread"]) or "none"))
    print("gaps Linux closes before an SDMA ring runs:")
    for g in out["gaps"] or ["none seen"]:
        print(f"  - {g}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
