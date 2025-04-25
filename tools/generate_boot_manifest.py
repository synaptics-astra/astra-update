# Generate Manifest files for boot images
# This script can be run as part of the build process using the uboot_binary,
# sdK_config, and uboot_config arguments.
# python3 generate_boot_manifest.py \
#            --uboot_binary ${B}/target/release/uboot/uboot_en.bin \
#            --sdk_config ${STAGING_DATADIR_NATIVE}/syna/build/.config \
#            --uboot_config ${B}/target/release/uboot/intermediate/output_uboot/.config \
#            --output ${WORKDIR}/manifest.yaml
# It can also be used standalone by supplying the values using the command line arguments.

import uuid
import argparse
import subprocess
import re

def extract_uboot_version(file_path):
    try:
        result = subprocess.run(['strings', file_path], capture_output=True, text=True, check=True)
        for line in result.stdout.splitlines():
            match = re.search(r'U-Boot\s+\d{4}\.\d{2}(?:-[\w\-]+)?\s+\([A-Za-z]{3}\s+\d{1,2}\s+\d{4}\s+-\s+\d{2}:\d{2}:\d{2}\s+\+\d{4}\)', line)
            if match:
                return match.group(0).strip()

    except subprocess.CalledProcessError:
        pass
    return ''

def parse_sdk_config(file_path):
    secure_boot = 'gen2'
    memory_layout = None
    uboot = 'uboot'
    chip = None
    board = None
    try:
        with open(file_path) as f:
            for line in f:
                if 'CONFIG_GENX_ENABLE=y' in line:
                    secure_boot = 'genx'
                elif 'CONFIG_BERLIN_DOLPHIN_A0=y' in line:
                    chip = 'sl1680'
                elif 'CONFIG_BERLIN_PLATYPUS_A0=y' in line:
                    chip = 'sl1640'
                elif 'CONFIG_BERLIN_MYNA2_A0=y' in line:
                    chip = 'sl1620'
                elif 'CONFIG_BOARD_NAME' in line and 'RDK' in line:
                    board = 'rdk'
                elif 'CONFIG_UBOOT_SUBOOT=y' in line:
                        uboot = 'suboot'
                else:
                    match = re.search(r'CONFIG_PREBOOT_MEMORY_SIZE="(\d+GB)"', line)
                    if match:
                        memory_layout = match.group(1).lower()
    except FileNotFoundError:
        pass
    return secure_boot, memory_layout, uboot, chip, board

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
    parser = argparse.ArgumentParser(description="Generate a manifest.yaml file for a boot image.")
    parser.add_argument('--chip')
    parser.add_argument('--board')
    parser.add_argument('--secure_boot')
    parser.add_argument('--vendor_id', default='06CB')
    parser.add_argument('--product_id')
    parser.add_argument('--console')
    parser.add_argument('--uenv_support')
    parser.add_argument('--memory_layout')
    parser.add_argument('--uboot')
    parser.add_argument('--uboot_version', default='', help='Full U-Boot version string (quoted if it has spaces)')
    parser.add_argument('--uboot_binary', help='Path to U-Boot binary to extract version from')
    parser.add_argument('--sdk_config', help='Path to SDK config file')
    parser.add_argument('--uboot_config', help='Path to U-Boot config file')
    parser.add_argument('--output', default='manifest.yaml', help='Path to output Manifest file')

    args = parser.parse_args()

    unique_id = str(uuid.uuid1())

    uboot_version = args.uboot_version
    if not uboot_version and args.uboot_binary:
        uboot_version = extract_uboot_version(args.uboot_binary)

    secure_boot = args.secure_boot
    memory_layout = args.memory_layout
    uboot = args.uboot
    chip = args.chip
    board = args.board
    if args.sdk_config:
        sb, ml, ub, cp, bd = parse_sdk_config(args.sdk_config)
        secure_boot = secure_boot or sb
        memory_layout = memory_layout or ml
        uboot = uboot or ub
        chip = chip or cp
        board = board or bd

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
uboot: {uboot}
uboot_version: \"{uboot_version}\"
"""

    with open(args.output, 'w') as file:
        file.write(yaml_content)

if __name__ == '__main__':
    main()
