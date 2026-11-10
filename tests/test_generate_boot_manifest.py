# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Synaptics Incorporated

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


GENERATOR = Path(__file__).resolve().parents[1] / 'tools' / 'generate_boot_manifest.py'


class GenerateBootManifestTests(unittest.TestCase):
    def test_xspi_support_true_false_and_unspecified(self):
        for chip in ('sl1680', 'sl2619'):
            for support in ('true', 'false', None):
                with self.subTest(chip=chip, xspi_support=support), tempfile.TemporaryDirectory() as directory:
                    output = Path(directory) / 'manifest.yaml'
                    command = [
                        sys.executable, str(GENERATOR),
                        '--chip', chip, '--board', 'rdk', '--secure_boot', 'genx',
                        '--console', 'uart', '--uenv_support', 'true',
                        '--memory_layout', '2gb', '--ddr-type', 'ddr4',
                        '--uboot', 'suboot', '--output', str(output),
                    ]
                    if support is not None:
                        command.extend(['--xspi_support', support])
                    result = subprocess.run(command, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    fields = dict(line.split(': ', 1) for line in output.read_text().splitlines())
                    self.assertEqual(fields['chip'], chip)
                    self.assertEqual(fields['memory_layout'], '2gb')
                    if support is None:
                        self.assertNotIn('xspi_support', fields)
                    else:
                        self.assertEqual(fields['xspi_support'], support)
                    if chip == 'sl2619':
                        self.assertEqual(fields['nand_support'], 'false')
                        self.assertEqual(fields['transport'], 'usb_cdc')

    def test_invalid_xspi_support_is_rejected_without_writing_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'manifest.yaml'
            result = subprocess.run(
                [sys.executable, str(GENERATOR), '--xspi_support', 'maybe', '--output', str(output)],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn('--xspi_support', result.stderr)
            self.assertIn('invalid choice', result.stderr)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
