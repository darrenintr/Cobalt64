"""Offline Queue1 census decoding; never opens device files."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "queue1_census", Path(__file__).resolve().parents[2] / "tools/queue1-census.py")
census = importlib.util.module_from_spec(spec)
spec.loader.exec_module(census)


def window(name, ip, seg, first, words):
    rows = [f"== {name}  {ip} seg{seg} reg 0x{first:04x} x{len(words)}"]
    for i in range(0, len(words), 8):
        rows.append(f"  0x{first + i:04x}: " +
                    " ".join(f"{w:08x}" for w in words[i:i + 8]))
    return "\n".join(rows)


def results(rlc, f32=None, gc_l1=None, sos=0x0b88b410):
    psp = [0] * 96
    psp[49], psp[26] = sos, 0x00330024
    parts = [
        "== n33peek --all (allowlisted BAR5 register windows)",
        window("PSP_C2PMSG", "MP0", 0, 0x60, psp),
        window("SDMA0_ID", "GC", 0, 0x34, [1, 0x600]),
        window("SDMA0_Q1_RB", "GC", 0, 0xd8, [0x00040800] + [0] * 6),
        window("SDMA0_Q1_CTL", "GC", 0, 0xe0, [0, 0, 0x100] + [0] * 6 + [0x804, 0]),
        window("GCVM_L2_CNTL", "GC", 0, 0x15bc, [0x00080602]),
        window("GCVM_CONTEXT_CNTL", "GC", 0, 0x1688, [0x01fffe00] * 16),
        window("IMU_GFX_RESET_CTRL", "GC", 1, 0x40bc, [0x1f]),
        window("RLC_BOOTLOAD_STATUS", "GC", 1, 0x4e82, [rlc]),
        window("MMVM_L2_CNTL", "MMHUB", 0, 0x680, [0x00080602]),
        window("MMVM_CONTEXT_CNTL", "MMHUB", 0, 0x6c0, [0x01fffe00] * 16),
    ]
    if f32 is not None:
        parts.append(window("SDMA0_F32_CNTL", "GC", 1, 0x589a, [f32]))
    if gc_l1 is not None:
        parts.append(window("GCMC_VM_MX_L1_TLB_CNTL", "GC", 0, 0x167f, [gc_l1]))
    parts.append("GPU writes: NONE; allowlisted BAR5 reads only.")
    return "\n".join(parts)


class CensusTests(unittest.TestCase):
    def test_after_scpm_hubs_off(self):
        out = census.census(census.parse(results(0xc000001f)))
        self.assertTrue(out["sos_alive"])
        self.assertEqual(out["rlc_bootload_complete"], 1)
        self.assertEqual(out["gfxhub"]["l2_cache_enabled"], 0)
        self.assertEqual(out["q1_vmid"], 0)
        self.assertIn("gfxhub L2 cache off (GCVM_L2_CNTL.ENABLE_L2_CACHE)", out["gaps"])
        self.assertIn("sdma_f32_cntl", out["unread"])

    def test_new_windows_decode(self):
        out = census.census(census.parse(results(0xc000001f, f32=1, gc_l1=0x1859)))
        self.assertEqual(out["sdma_halted"], 1)
        self.assertEqual(out["gfxhub"]["l1_tlb_enabled"], 1)
        self.assertEqual(out["gfxhub"]["system_access_mode"], 3)
        self.assertEqual(out["gfxhub"]["advanced_driver_model"], 1)
        self.assertIn("SDMA0_F32_CNTL.HALT set (sdma_v6_0 clears it)", out["gaps"])
        self.assertNotIn("sdma_f32_cntl", out["unread"])

    def test_gc_zeros_ignored_while_gc_down(self):
        out = census.census(census.parse(results(0, f32=1, sos=0)))
        self.assertFalse(out["gc_values_meaningful"])
        self.assertFalse(out["sos_alive"])
        self.assertFalse(any("gfxhub" in g or "SDMA" in g for g in out["gaps"]))
        self.assertIn("mmhub L2 cache off (MMVM_L2_CNTL.ENABLE_L2_CACHE)", out["gaps"])


if __name__ == "__main__":
    unittest.main()
