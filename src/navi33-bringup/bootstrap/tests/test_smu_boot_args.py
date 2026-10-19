"""Exercise the actual OpenCore config generator without disks or hardware."""
import pathlib
import plistlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / 'vm/make-opencore.sh'


class SmuBootArgsTests(unittest.TestCase):
    def generate(self, load=False, ring=False, fw=False, autoload=False,
                 smu=False, scpm=False, dma_preflight=False, mes=False, mec=False, grad=False, auto=False):
        code = SCRIPT.read_text().split("<<'EOF'\n", 1)[1].split('\nEOF', 1)[0]
        guid = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            config = tmp / 'config.plist'
            # Start with stale SMU opt-in: generator must strip all navi33 args.
            data = {'Kernel': {'Add': []}, 'NVRAM': {
                'Add': {guid: {'boot-args': 'keepsyms=1 navi33-smu-enable=1 navi33-smu-scpm=1'}},
                'Delete': {guid: ['boot-args']}},
                'Misc': {'Boot': {}, 'Debug': {'Target': 0}}}
            config.write_bytes(plistlib.dumps(data))
            binary = tmp / 'binary'
            binary.write_bytes(bytes(32))  # no LC_UUID: harmless manifest fallback
            args = [str(config), str(int(load)), str(binary), str(tmp / 'manifest'),
                    'test-OC', '', str(int(ring)), str(int(fw)), str(int(autoload)),
                    str(int(smu or scpm)), str(int(scpm)), str(int(dma_preflight)),
                    '0', '0', '0', '0', str(int(mes)), str(int(mec)), str(int(grad)), str(int(auto))]
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
        self.assertNotIn('navi33-smu-scpm=1', args)

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

    def test_scpm_opens_only_enable_only_gate(self):
        args = self.generate(scpm=True)
        self.assertIn('navi33-smu-enable=1', args)
        self.assertIn('navi33-smu-scpm=1', args)
        for flag in ('navi33-smu-off=1', 'navi33-psp-off=1',
                     'navi33-psp-load=1', 'navi33-psp-ring=1', 'navi33-psp-fw=1'):
            self.assertNotIn(flag, args)
        self.assertNotIn('navi33-smu-scpm=1', self.generate(smu=True))

    def test_mes_opens_only_mes(self):
        args = self.generate(mes=True)
        self.assertIn('navi33-mes-start=1', args)
        self.assertIn('navi33-smu-off=1', args)
        for flag in ('navi33-mes-off=1', 'navi33-psp-off=1', 'navi33-smu-enable=1',
                     'navi33-psp-load=1', 'navi33-psp-ring=1', 'navi33-psp-fw=1'):
            self.assertNotIn(flag, args)
        for closed in (self.generate(), self.generate(smu=True), self.generate(fw=True)):
            self.assertIn('navi33-mes-off=1', closed)
            self.assertNotIn('navi33-mes-start=1', closed)

    def test_mec_opens_only_mec(self):
        args = self.generate(mec=True)
        self.assertIn('navi33-mec-queue=1', args)
        for flag in ('navi33-mes-off=1', 'navi33-smu-off=1'):
            self.assertIn(flag, args)
        for flag in ('navi33-mec-off=1', 'navi33-psp-off=1', 'navi33-mes-start=1',
                     'navi33-smu-enable=1', 'navi33-psp-load=1'):
            self.assertNotIn(flag, args)
        for closed in (self.generate(), self.generate(mes=True), self.generate(fw=True)):
            self.assertIn('navi33-mec-off=1', closed)
            self.assertNotIn('navi33-mec-queue=1', closed)

    def test_gradient_opens_only_gradient(self):
        args = self.generate(grad=True)
        self.assertIn('navi33-gradient=1', args)
        for flag in ('navi33-mec-off=1', 'navi33-mes-off=1', 'navi33-smu-off=1'):
            self.assertIn(flag, args)
        for flag in ('navi33-gradient-off=1', 'navi33-psp-off=1', 'navi33-mec-queue=1'):
            self.assertNotIn(flag, args)
        for closed in (self.generate(), self.generate(mec=True)):
            self.assertIn('navi33-gradient-off=1', closed)
            self.assertNotIn('navi33-gradient=1', closed)

    def test_autostart_drops_only_its_stages_kill_switches(self):
        args = self.generate(auto=True)
        self.assertIn('navi33-autostart=1', args)
        for flag in ('navi33-psp-off=1', 'navi33-smu-off=1', 'navi33-mes-off=1', 'navi33-mec-off=1',
                     'navi33-psp-load=1', 'navi33-smu-enable=1', 'navi33-mec-queue=1'):
            self.assertNotIn(flag, args)
        self.assertIn('navi33-gradient-off=1', args)   # not part of the bring-up
        for closed in (self.generate(), self.generate(mec=True), self.generate(grad=True)):
            self.assertNotIn('navi33-autostart=1', closed)

    def test_mes_refuses_combined_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            for gate, other in (('--psp-load', '--mes-start'), ('--psp-ring', '--mes-start'),
                                ('--psp-fw', '--mes-start'), ('--smu-scpm', '--mes-start'),
                                ('--mes-start', '--mec-queue'), ('--psp-fw', '--mec-queue'),
                                ('--mec-queue', '--gradient'), ('--autostart', '--psp-load')):
                result = subprocess.run(['bash', str(SCRIPT), directory,
                                         gate, other], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('open one write gate at a time', result.stderr)

    def test_dma_preflight_opens_only_read_only_mapping_probe(self):
        args = self.generate(dma_preflight=True)
        self.assertIn('navi33-dma-preflight=1', args)
        self.assertNotIn('navi33-dma-off=1', args)
        self.assertIn('navi33-psp-off=1', args)
        self.assertIn('navi33-smu-off=1', args)

    def test_combined_write_gates_are_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            for gate in ('--psp-load', '--psp-ring', '--psp-fw'):
                result = subprocess.run(['bash', str(SCRIPT), directory,
                                         gate, '--smu-enable'], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('open one write gate at a time', result.stderr)

    def test_scpm_refuses_combined_psp_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            for gate in ('--psp-load', '--psp-ring', '--psp-fw'):
                result = subprocess.run(['bash', str(SCRIPT), directory,
                                         gate, '--smu-scpm'], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('open one write gate at a time', result.stderr)
