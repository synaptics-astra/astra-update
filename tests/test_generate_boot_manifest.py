# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Synaptics Incorporated

import contextlib
import io
import runpy
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


GENERATOR = Path(__file__).resolve().parents[1] / 'tools' / 'generate_boot_manifest.py'

def run_generator(arguments):
    stdout = io.StringIO()
    stderr = io.StringIO()
    original_argv = sys.argv
    try:
        sys.argv = [str(GENERATOR), *arguments]
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            try:
                runpy.run_path(str(GENERATOR), run_name='__main__')
                returncode = 0
            except SystemExit as error:
                returncode = error.code if isinstance(error.code, int) else 1
    finally:
        sys.argv = original_argv
    return SimpleNamespace(returncode=returncode, stdout=stdout.getvalue(), stderr=stderr.getvalue())


class GenerateBootManifestTests(unittest.TestCase):
    def test_xspi_support_true_false_and_unspecified(self):
        for chip in ('sl1680', 'sl2619'):
            for support in ('true', 'false', None):
                with self.subTest(chip=chip, xspi_support=support), tempfile.TemporaryDirectory() as directory:
                    output = Path(directory) / 'manifest.yaml'
                    arguments = [
                        '--chip', chip, '--board', 'rdk', '--secure_boot', 'genx',
                        '--console', 'uart', '--uenv_support', 'true',
                        '--memory_layout', '2gb', '--ddr-type', 'ddr4',
                        '--uboot', 'suboot', '--output', str(output),
                    ]
                    if support is not None:
                        arguments.extend(['--xspi_support', support])
                    result = run_generator(arguments)
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
            result = run_generator(['--xspi_support', 'maybe', '--output', str(output)])
            self.assertEqual(result.returncode, 2)
            self.assertIn('--xspi_support', result.stderr)
            self.assertIn('invalid choice', result.stderr)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
