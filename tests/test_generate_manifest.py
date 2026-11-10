# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Synaptics Incorporated

import subprocess
import sys
import tempfile
import unittest
import json
from pathlib import Path


GENERATOR = Path(__file__).resolve().parents[1] / 'tools' / 'generate_manifest.py'


class GenerateManifestTests(unittest.TestCase):
    def generate_update(self, directory, image_type, *options):
        output = Path(directory) / 'manifest.yaml'
        result = subprocess.run([
            sys.executable, str(GENERATOR), '--image_type', image_type,
            '--chip', 'sl2619', '--board', 'coralboard', '--secure_boot', 'genx',
            '--memory_layout', '2gb', '--ddr-type', 'ddr4x16',
            '--output', str(output), *options,
        ], capture_output=True, text=True)
        return result, output

    def update_fields(self, output):
        return {
            key: json.loads(value)
            for key, value in (line.split(': ', 1) for line in output.read_text().splitlines()
                               if line and not line.startswith(' ') and line != 'images:')
        }

    def test_update_manifest_shapes_and_payloads(self):
        cases = (
            ('spi', ('--image_file', 'spi_suboot.bin', '--image_file', 'boot.subimg')),
            ('emmc', ()),
            ('nand', ()),
            ('nand', ('--image_file', 'custom.img')),
        )
        for image_type, options in cases:
            with self.subTest(image_type=image_type, options=options), tempfile.TemporaryDirectory() as directory:
                result, output = self.generate_update(directory, image_type, *options)
                self.assertEqual(result.returncode, 0, result.stderr)
                expected = {
                    'image_type': image_type, 'chip': 'sl2619', 'board': 'coralboard',
                    'secure_boot': 'genx', 'memory_layout': '2gb', 'ddr_type': 'ddr4x16',
                    'reset': 'enable',
                }
                if image_type == 'nand':
                    expected['image_file'] = options[1] if options else 'uNAND_full.img'
                self.assertEqual(self.update_fields(output), expected)
                image_lines = [line for line in output.read_text().splitlines() if line.startswith(' ')]
                if image_type == 'spi':
                    self.assertEqual(image_lines, [
                        '    "spi_suboot.bin": {}', '    "boot.subimg": {}',
                    ])
                else:
                    self.assertEqual(image_lines, [])

    def test_update_boot_id_and_reset_override(self):
        with tempfile.TemporaryDirectory() as directory:
            result, output = self.generate_update(
                directory, 'emmc', '--boot_image', 'boot-id', '--reset', 'disable')
            self.assertEqual(result.returncode, 0, result.stderr)
            fields = self.update_fields(output)
            self.assertEqual(fields['boot_image'], 'boot-id')
            self.assertEqual(fields['reset'], 'disable')

    def test_explicit_boot_mode_preserves_default_manifest_shape(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'manifest.yaml'
            command = [
                sys.executable, str(GENERATOR), '--chip', 'sl2619',
                '--board', 'coralboard', '--secure_boot', 'genx',
                '--memory_layout', '2gb', '--ddr-type', 'ddr4x16',
                '--output', str(output),
            ]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            default_fields = dict(line.split(': ', 1) for line in output.read_text().splitlines())
            result = subprocess.run(command + ['--image_type', 'boot'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            explicit_fields = dict(line.split(': ', 1) for line in output.read_text().splitlines())
            default_fields.pop('id')
            explicit_fields.pop('id')
            self.assertEqual(default_fields, explicit_fields)
            self.assertNotIn('image_type', explicit_fields)
            self.assertEqual(explicit_fields['fastboot_product_id'], '4EE0')

    def test_optional_ddr_and_quoted_board(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'manifest.yaml'
            result = subprocess.run([
                sys.executable, str(GENERATOR), '--image_type', 'emmc',
                '--chip', 'sl2619', '--board', 'board: "custom"',
                '--secure_boot', 'genx', '--memory_layout', '2gb',
                '--output', str(output),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            fields = self.update_fields(output)
            self.assertNotIn('ddr_type', fields)
            self.assertEqual(fields['board'], 'board: "custom"')

    def test_sdk_config_supplies_update_metadata_with_cli_precedence(self):
        for image_type in ('spi', 'emmc', 'nand'):
            with self.subTest(image_type=image_type), tempfile.TemporaryDirectory() as directory:
                config = Path(directory) / 'sdk.config'
                config.write_text(
                    'CONFIG_PRODUCT_NAME="sl2619_aarch64_coralboard"\n'
                    'CONFIG_GENX_ENABLE=y\nCONFIG_PREBOOT_MEMORY_SIZE="2GB"\n'
                    'CONFIG_PREBOOT_DDR_TYPE="DDR4X16"\n')
                output = Path(directory) / 'manifest.yaml'
                command = [
                    sys.executable, str(GENERATOR), '--image_type', image_type,
                    '--sdk_config', str(config), '--memory_layout', '1gb',
                    '--output', str(output),
                ]
                if image_type == 'spi':
                    command.extend(['--image_file', 'spi_suboot.bin'])
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                fields = self.update_fields(output)
                self.assertEqual(fields['chip'], 'sl2619')
                self.assertEqual(fields['board'], 'coralboard')
                self.assertEqual(fields['secure_boot'], 'genx')
                self.assertEqual(fields['memory_layout'], '1gb')
                self.assertEqual(fields['ddr_type'], 'ddr4x16')

    def test_invalid_update_inputs_do_not_write_manifest(self):
        cases = (
            ('spi', ()),
            ('spi', ('--image_file', '../payload.bin')),
            ('spi', ('--image_file', 'a.bin', '--image_file', 'a.bin')),
            ('spi', ('--image_file', 'bad file.bin')),
            ('emmc', ('--image_file', 'payload.bin')),
            ('nand', ('--image_file', 'a.img', '--image_file', 'b.img')),
        )
        for image_type, options in cases:
            with self.subTest(image_type=image_type, options=options), tempfile.TemporaryDirectory() as directory:
                result, output = self.generate_update(directory, image_type, *options)
                self.assertEqual(result.returncode, 2)
                self.assertIn('error:', result.stderr)
                self.assertFalse(output.exists())

    def test_missing_update_metadata_is_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'manifest.yaml'
            result = subprocess.run([
                sys.executable, str(GENERATOR), '--image_type', 'emmc', '--output', str(output),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('chip, board, secure_boot, memory_layout', result.stderr)
            self.assertFalse(output.exists())

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
