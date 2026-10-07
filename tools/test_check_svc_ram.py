"""Regression checks for direct calls, linker veneers, and unknown BLX."""
import contextlib
import io
from pathlib import Path
import runpy
import sys
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).with_name('check_svc_ram.py')

class AuditTests(unittest.TestCase):
    def audit(self, disassembly):
        output = io.StringIO()
        with patch.object(sys, 'argv', [str(SCRIPT), 'fixture.elf']), \
             patch('subprocess.check_output', return_value=disassembly), \
             contextlib.redirect_stdout(output), self.assertRaises(SystemExit) as end:
            runpy.run_path(str(SCRIPT), run_name='__main__')
        return end.exception.code, output.getvalue()

    def test_empty_disassembly_does_not_pass(self):
        code, _ = self.audit('')
        self.assertEqual(code, 2)

    def test_ram_call_passes(self):
        code, out = self.audit('20000100 <shizuku_svc_dispatch>:\n'
                              ' 20000100: f000 f800 bl 20000200 <shizuku_current_context>\n'
                              '20000200 <shizuku_current_context>:\n'
                              ' 20000200: 4770 bx lr\n')
        self.assertEqual(code, 0)
        self.assertIn('flash_edges=0', out)

    def test_direct_flash_call_fails(self):
        code, out = self.audit('20000100 <shizuku_svc_dispatch>:\n'
                              ' 20000100: f000 f800 bl 10000200 <memcpy>\n')
        self.assertEqual(code, 1)
        self.assertIn('flash_edges=1', out)

    def test_ram_veneer_to_flash_fails(self):
        code, out = self.audit('20000100 <shizuku_svc_dispatch>:\n'
                              ' 20000100: f000 f800 bl 20000200 <__memcpy_veneer>\n'
                              '20000200 <__memcpy_veneer>:\n'
                              ' 20000200: f85f f000 ldr.w pc, [pc]\n'
                              ' 20000204: 10000201 .word 0x10000201\n')
        self.assertEqual(code, 1)
        self.assertIn('0x10000201', out)

    def test_unknown_indirect_call_does_not_pass(self):
        code, out = self.audit('20000100 <shizuku_svc_dispatch>:\n'
                              ' 20000100: 4798 blx r3\n')
        self.assertEqual(code, 2)
        self.assertIn('indirect_calls=1', out)

if __name__ == '__main__':
    unittest.main()
