"""Exercise the actual OpenCore config generator without disks or hardware."""
import pathlib
import plistlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / 'vm/make-opencore.sh'


class SmuBootArgsTests(unittest.TestCase):
    def generate(self, load=False, ring=False, fw=False, autoload=False, smu=False):
        code = SCRIPT.read_text().split("<<'EOF'\n", 1)[1].split('\nEOF', 1)[0]
        guid = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            config = tmp / 'config.plist'
            # Start with stale SMU opt-in: generator must strip all navi33 args.
            data = {'Kernel': {'Add': []}, 'NVRAM': {
                'Add': {guid: {'boot-args': 'keepsyms=1 navi33-smu-enable=1'}},
                'Delete': {guid: ['boot-args']}},
                'Misc': {'Boot': {}, 'Debug': {'Target': 0}}}
            config.write_bytes(plistlib.dumps(data))
            binary = tmp / 'binary'
            binary.write_bytes(bytes(32))  # no LC_UUID: harmless manifest fallback
            args = [str(config), str(int(load)), str(binary), str(tmp / 'manifest'),
                    'test-OC', '', str(int(ring)), str(int(fw)), str(int(autoload)), str(int(smu))]
            subprocess.run([sys.executable, '-I', '-', *args], input=code,
                           text=True, capture_output=True, check=True)
            result = plistlib.loads(config.read_bytes())
            return result['NVRAM']['Add'][guid]['boot-args'].split()

    def test_default_is_closed(self):
        args = self.generate()
        self.assertIn('navi33-psp-off=1', args)
        self.assertIn('navi33-smu-off=1', args)
        self.assertNotIn('navi33-smu-enable=1', args)
        self.assertIn('keepsyms=1', args)

    def test_psp_modes_keep_smu_closed(self):
        for mode in ('load', 'ring', 'fw'):
            with self.subTest(mode=mode):
                args = self.generate(**{mode: True})
                self.assertIn('navi33-smu-off=1', args)
                self.assertNotIn('navi33-smu-enable=1', args)
                self.assertNotIn('navi33-psp-off=1', args)

    def test_smu_opens_only_smu(self):
        args = self.generate(smu=True)
        self.assertIn('navi33-smu-enable=1', args)
        for flag in ('navi33-smu-off=1', 'navi33-psp-off=1',
                     'navi33-psp-load=1', 'navi33-psp-ring=1', 'navi33-psp-fw=1'):
            self.assertNotIn(flag, args)

    def test_combined_write_gates_are_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            for gate in ('--psp-load', '--psp-ring', '--psp-fw'):
                result = subprocess.run(['bash', str(SCRIPT), directory,
                                         gate, '--smu-enable'], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('open one write gate at a time', result.stderr)
