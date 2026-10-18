"""Offline PSP runtime DB decoding; never opens device files."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location(
    "scpm_read", Path(__file__).resolve().parents[2] / "tools/scpm-read.py")
scpm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scpm)


def fixture(status=1):
    data = bytearray(scpm.DB_SIZE)
    struct.pack_into("<HHH", data, 0, scpm.COOKIE, scpm.VERSION, 2)
    # Match the C ABI, including nonzero directory alignment padding.
    struct.pack_into("<H", data, 6, 0xA55A)
    struct.pack_into("<IHH", data, 8, 5, 0x300, 8)
    struct.pack_into("<IHH", data, 16, 6, 0x400, 4)
    struct.pack_into("<II", data, 0x300, 3, 0)
    struct.pack_into("<I", data, 0x400, status)
    return data


class RuntimeDbTests(unittest.TestCase):
    def test_scpm_states_and_padded_directory(self):
        for status, meaning in enumerate(("disabled", "enabled", "enabled with error")):
            with self.subTest(status=status):
                out = scpm.parse_runtime_db(fixture(status))
                self.assertEqual(out["scpm_status"], status)
                self.assertEqual(out["scpm_meaning"], meaning)
                self.assertEqual(out["boot_config"], 3)

    def test_undocumented_status_is_preserved(self):
        out = scpm.parse_runtime_db(fixture(0x40000000))
        self.assertEqual(out["scpm_status"], 0x40000000)
        self.assertEqual(out["scpm_status_hex"], "0x40000000")
        self.assertEqual(out["scpm_meaning"], "unknown")
        self.assertTrue(out["scpm_enabled_by_amdgpu"])

    def test_absent_database_is_unknown(self):
        out = scpm.parse_runtime_db(bytes(scpm.DB_SIZE))
        self.assertFalse(out["present"])
        self.assertIsNone(out["scpm_status"])

    def test_missing_scpm_is_unknown(self):
        data = fixture()
        struct.pack_into("<H", data, 4, 1)
        self.assertIsNone(scpm.parse_runtime_db(data)["scpm_status"])

    def test_bad_versions_and_entry_count(self):
        for offset, fmt, value in ((2, "<H", 0x200), (4, "<H", 64)):
            with self.subTest(offset=offset):
                data = fixture()
                struct.pack_into(fmt, data, offset, value)
                with self.assertRaises(ValueError):
                    scpm.parse_runtime_db(data)

    def test_entry_cannot_read_header_or_past_capture(self):
        for offset, size in ((0, 4), (16, 4), (0xFFFE, 4), (0x400, 0), (0x400, 3)):
            with self.subTest(offset=offset, size=size):
                data = fixture()
                struct.pack_into("<IHH", data, 16, 6, offset, size)
                with self.assertRaises(ValueError):
                    scpm.parse_runtime_db(data)

    def test_duplicate_and_overlapping_entries(self):
        for kind, offset in ((5, 0x400), (6, 0x300)):
            with self.subTest(kind=kind, offset=offset):
                data = fixture()
                struct.pack_into("<IHH", data, 16, kind, offset, 8)
                with self.assertRaises(ValueError):
                    scpm.parse_runtime_db(data)

    def test_exact_capture_size_required(self):
        for size in (0, 6, scpm.DB_SIZE - 1, scpm.DB_SIZE + 1):
            with self.subTest(size=size), self.assertRaises(ValueError):
                scpm.parse_runtime_db(bytes(size))


if __name__ == "__main__":
    unittest.main()
