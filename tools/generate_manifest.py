# Generate manifest files for USB boot and SPI/eMMC/NAND update images.
# This script can be run as part of the build process using the uboot_binary,
# sdK_config, and uboot_config arguments.
# python3 generate_manifest.py \
#            --uboot_binary ${B}/target/release/uboot/uboot_en.bin \
#            --sdk_config ${STAGING_DATADIR_NATIVE}/syna/build/.config \
#            --uboot_config ${B}/target/release/uboot/intermediate/output_uboot/.config \
#            --output ${WORKDIR}/manifest.yaml
# It can also be used standalone by supplying the values using the command line arguments.

import uuid
import argparse
import subprocess
import re
import json


def extract_chip_from_product_name(line):
    match = re.search(r'CONFIG_PRODUCT_NAME="[^"]*(sl\d{4})[^"]*"', line, re.IGNORECASE)
    if match:
        return match.group(1).lower()
    return None


def extract_board_from_product_name(line):
    arch_tokens = {'aarch64', 'aarch32', 'arm64', 'armhf'}
    match = re.search(r'CONFIG_PRODUCT_NAME="([^"]+)"', line)
    if not match:
        return None

    product_name = match.group(1).lower()
    arch_match = re.search(r'_(aarch64|aarch32|arm64|armhf)_([^_"]+)$', product_name)
    if arch_match:
        return arch_match.group(2)

    parts = product_name.split('_')
    if len(parts) > 1 and re.fullmatch(r'sl\d{4}', parts[0]) and parts[-1] not in arch_tokens:
        return parts[-1]

    return None

def extract_uboot_version(file_path):
    try:
        result = subprocess.run(['strings', file_path], capture_output=True, text=True, check=True)
        for line in result.stdout.splitlines():
            match = re.search(r'^U-Boot\s+\d{4}\.\d{2}(?:[._-][A-Za-z0-9._-]+)*\s+\([A-Za-z]{3}\s+\d{1,2}\s+\d{4}\s+-\s+\d{2}:\d{2}:\d{2}\s+\+\d{4}\)$', line)
            if match:
                return match.group(0).strip()

    except subprocess.CalledProcessError:
        pass
    return ''

def parse_sdk_config(file_path):
    secure_boot = 'gen2'
    memory_layout = None
    ddr_type = None
    uboot = 'uboot'
    chip = None
    board = None
    try:
        with open(file_path) as f:
            for line in f:
                if 'CONFIG_PRODUCT_NAME' in line:
                    product_chip = extract_chip_from_product_name(line)
                    product_board = extract_board_from_product_name(line)
                    if product_chip:
                        chip = product_chip
                    if product_board:
                        board = product_board
                elif 'CONFIG_GENX_ENABLE=y' in line:
                    secure_boot = 'genx'
                elif 'CONFIG_BERLIN_DOLPHIN_A0=y' in line:
                    chip = 'sl1680'
                elif 'CONFIG_BERLIN_PLATYPUS_A0=y' in line:
                    chip = 'sl1640'
                elif 'CONFIG_BERLIN_MYNA2_A0=y' in line:
                    chip = 'sl1620'
                elif 'CONFIG_BERLIN_KLAMATH=y' in line:
                    # chip name should have already been extracted from CONFIG_PRODUCT_NAME
                    pass
                elif 'CONFIG_UBOOT_SUBOOT=y' in line:
                    uboot = 'suboot'
                else:
                    match = re.search(r'CONFIG_PREBOOT_MEMORY_SIZE="(\d+(?:MB|GB))"', line)
                    if match:
                        memory_layout = match.group(1).lower()
                    match = re.search(r'CONFIG_PREBOOT_DDR_TYPE="([\d\w]+)"', line)
                    if match:
                        ddr_type = match.group(1).lower()
    except FileNotFoundError:
        pass
    return secure_boot, memory_layout, ddr_type, uboot, chip, board

def parse_uboot_config(file_path):
    console = 'uart'
    uenv_support = 'false'
    try:
        with open(file_path) as f:
            for line in f:
                if 'CONFIG_USBCONSOLE=y' in line:
                    console = 'usb'
                if 'CONFIG_PREBOOT' in line and 'usbload uEnv.txt' in line:
                    uenv_support = 'true'
    except FileNotFoundError:
        pass
    return console, uenv_support

def main():
    parser = argparse.ArgumentParser(description="Generate a manifest.yaml file for a boot or update image.")
    parser.add_argument('--image_type', choices=('boot', 'spi', 'emmc', 'nand'), default='boot',
                        help='Manifest type (default: boot)')
    parser.add_argument('--image_file', action='append',
                        help='SPI or NAND payload filename (defaults: SPI spi_suboot.bin, NAND uNAND_full.img); '
                             'repeat for multiple SPI payloads')
    parser.add_argument('--boot_image', help='Optional boot image ID for update manifests')
    parser.add_argument('--reset', choices=('enable', 'disable'), default='enable',
                        help='Reset after an update (default: enable)')
    parser.add_argument('--chip')
    parser.add_argument('--board')
    parser.add_argument('--secure_boot')
    parser.add_argument('--vendor_id', default='06CB')
    parser.add_argument('--product_id')
    parser.add_argument('--sysmgr_vendor_id', default=None, help='SysMgr USB vendor ID (hex, e.g. 06CB)')
    parser.add_argument('--sysmgr_product_id', default=None, help='SysMgr USB product ID (hex)')
    parser.add_argument('--fastboot_vendor_id', default=None, help='Fastboot USB vendor ID (hex, e.g. 18D1)')
    parser.add_argument('--fastboot_product_id', default=None, help='Fastboot USB product ID (hex, e.g. D00D)')
    parser.add_argument('--console')
    parser.add_argument('--uenv_support')
    parser.add_argument('--nand_support', default=None, help='Whether NAND boot is supported (true/false)')
    parser.add_argument('--xspi_support', choices=('true', 'false'), default=None,
                        help='Whether XSPI flash updates are supported (true/false); omitted if unspecified')
    parser.add_argument('--fastboot_reboot', choices=('true', 'false'), default='true',
                        help='Re-enter fastboot after flashing for host-controlled reboot '
                             '(default: true); set false for older U-Boot builds')
    parser.add_argument('--transport', default=None, help='Boot image transport type (usb or usb_cdc)')
    parser.add_argument('--memory_layout')
    parser.add_argument('--ddr-type')
    parser.add_argument('--uboot')
    parser.add_argument('--uboot_version', default='', help='Full U-Boot version string (quoted if it has spaces)')
    parser.add_argument('--uboot_binary', help='Path to U-Boot binary to extract version from')
    parser.add_argument('--sdk_config', help='Path to SDK config file')
    parser.add_argument('--uboot_config', help='Path to U-Boot config file')
    parser.add_argument('--output', default='manifest.yaml', help='Path to output Manifest file')

    args = parser.parse_args()

    secure_boot = args.secure_boot
    memory_layout = args.memory_layout
    ddr_type = args.ddr_type
    uboot = args.uboot
    chip = args.chip
    board = args.board
    if args.sdk_config:
        sb, ml, dt, ub, cp, bd = parse_sdk_config(args.sdk_config)
        secure_boot = secure_boot or sb
        memory_layout = memory_layout or ml
        ddr_type = ddr_type or dt
        uboot = uboot or ub
        chip = chip or cp
        board = board or bd

    if args.image_type != 'boot':
        missing = [name for name, value in (
            ('chip', chip), ('board', board), ('secure_boot', secure_boot),
            ('memory_layout', memory_layout),
        ) if not value]
        if missing:
            parser.error('Required update manifest values are missing: ' + ', '.join(missing))
        if args.image_type == 'emmc' and args.image_file:
            parser.error('eMMC manifests use emmc_part_list, not --image_file')
        if args.image_type == 'nand' and args.image_file and len(args.image_file) != 1:
            parser.error('NAND manifests accept only one --image_file')
        if args.image_file:
            if len(set(args.image_file)) != len(args.image_file):
                parser.error('Duplicate --image_file values are not allowed')
            for name in args.image_file:
                if (name in ('.', '..') or len(name) > 255
                        or re.fullmatch(r'[A-Za-z0-9._+-]+', name) is None):
                    parser.error('Invalid payload filename: ' + name)

        fields = {
            'image_type': args.image_type,
            'chip': chip,
            'board': board,
            'secure_boot': secure_boot,
            'memory_layout': memory_layout,
        }
        if ddr_type:
            fields['ddr_type'] = ddr_type
        if args.boot_image:
            fields['boot_image'] = args.boot_image
        fields['reset'] = args.reset
        if args.image_type == 'nand':
            fields['image_file'] = args.image_file[0] if args.image_file else 'uNAND_full.img'
        yaml_content = ''.join(f'{key}: {json.dumps(value)}\n' for key, value in fields.items())
        if args.image_type == 'spi':
            yaml_content += 'images:\n'
            for name in args.image_file or ['spi_suboot.bin']:
                yaml_content += f'    {json.dumps(name)}: {{}}\n'
        with open(args.output, 'w') as file:
            file.write(yaml_content)
        return

    if args.image_file or args.boot_image:
        parser.error('--image_file and --boot_image are only supported for update manifests')

    unique_id = str(uuid.uuid1())
    uboot_version = args.uboot_version
    if not uboot_version and args.uboot_binary:
        uboot_version = extract_uboot_version(args.uboot_binary)

    console = args.console
    uenv_support = args.uenv_support
    if args.uboot_config:
        con, uenv = parse_uboot_config(args.uboot_config)
        console = console or con
        uenv_support = uenv_support or uenv

    product_id = args.product_id
    if not product_id:
        if chip == "sl1680":
            product_id = "00B1"
        elif chip == "sl1640":
            product_id = "00B0"
        elif chip == "sl1620":
            product_id = "00B2"
        elif chip and chip.startswith("sl26"):
            product_id = "019E"

    sysmgr_vendor_id = args.sysmgr_vendor_id
    sysmgr_product_id = args.sysmgr_product_id
    fastboot_vendor_id = args.fastboot_vendor_id
    fastboot_product_id = args.fastboot_product_id
    nand_support = args.nand_support
    transport = args.transport

    if chip and chip.startswith("sl26"):
        sysmgr_vendor_id = sysmgr_vendor_id or "CAFE"
        sysmgr_product_id = sysmgr_product_id or "4002"
        fastboot_vendor_id = fastboot_vendor_id or "18D1"
        fastboot_product_id = fastboot_product_id or "4EE0"
        console = console or "uart"
        uenv_support = uenv_support or "true"
        nand_support = nand_support or "false"
        transport = transport or "usb_cdc"

    if not chip or not secure_boot or not product_id or not console:
        print("Required value is missing")
        return

    yaml_content = f"""id: {unique_id}
chip: {chip}
board: {board}
secure_boot: {secure_boot}
vendor_id: {args.vendor_id}
product_id: {product_id}
console: {console}
uenv_support: {uenv_support}
memory_layout: {memory_layout}
ddr_type: {ddr_type}
uboot: {uboot}
uboot_version: \"{uboot_version}\"
"""

    if sysmgr_vendor_id and sysmgr_product_id:
        yaml_content += f"sysmgr_vendor_id: {sysmgr_vendor_id}\n"
        yaml_content += f"sysmgr_product_id: {sysmgr_product_id}\n"

    if fastboot_vendor_id and fastboot_product_id:
        yaml_content += f"fastboot_vendor_id: {fastboot_vendor_id}\n"
        yaml_content += f"fastboot_product_id: {fastboot_product_id}\n"

    if nand_support is not None:
        yaml_content += f"nand_support: {nand_support}\n"

    if args.xspi_support is not None:
        yaml_content += f"xspi_support: {args.xspi_support}\n"

    yaml_content += f"fastboot_reboot: {args.fastboot_reboot}\n"

    if transport:
        yaml_content += f"transport: {transport}\n"

    with open(args.output, 'w') as file:
        file.write(yaml_content)

if __name__ == '__main__':
    main()
