# Astra Update

The Astra Update tool and library support communicating with Astra Machina devices over USB. The USB interface is
primarily used for updating the internal storage (eMMC / SPI / NAND). Support for booting Linux using the USB interface is also supported. Astra Update can run on Windows, Linux, and Mac OS platforms.

Pre-built versions of ``astra-update`` is located at https://github.com/synaptics-astra/usb-tool. In addition to pre-built binaries, this repo includes boot images and convenience scripts for running the tool.

## Overview

Astra Machina devices support booting and internal storage updates over USB. To enter USB Boot Mode, hold the USB_BOOT button while pressing the reset button. Once in this mode, the host detects the device and establishes a connection. The device then sends image requests through a USB interrupt endpoint, and the host responds by transferring images via a USB bulk endpoint. This exchange continues until the boot or update process is complete.

### Boot Images

Boot images are specific to the Astra SoC and board. Astra Update imports a collection of boot images which it can then match with update image to determine which boot image should be used and which SoC to connect to. Each set of boot images contains a ``manifest.yaml`` file which describes the boot image. Each ``manifest.yaml`` file contains an ID which distinguishes it from the other boot images.

Update images can also contain their own ``manifest.yaml`` file which specifies which boot image it requires. The boot image ID can also be set as a command line parameter. If an update image does not specify a boot image ID, then Astra Update can try to determine the correct boot image based on the characteristics of the update image. Manifest files are required for boot images, but are optional for update images.

### The Temp Directory

Astra Update creates a temp directory to store logs and supporting files which are used during the boot and update processes. By default, the temp directory will be created in the system default temp directory (``/tmp`` on Linux and Mac OS, ``C:\Users\<Username>\AppData\Local\Temp\`` on Windows). The temp directory contains the ``astradevicemanager.log`` file containing all logs for the execution of the process. The directory also contains device specific sub-directories which contains supporting data for each Astra device detected. The path of the temp directory can also be specified on the command line.

If the tool created the directory in the system default path, then it will delete it when the tool exits successfully. If the tool detects an error it will print the path of the directory and retain. The log file inside will contain additional information about the error. If the temp directory is specified on the command line then the tool will not automatically delete it.

## Usage

The ``astra-update`` tool is a command line utility used for updating the internal storage on Astra Machina. This section discusses the modes and command line parameters which it supports.

``astra-update`` needs a directory of boot images and an update image which will be used to update the board. The default set of boot images can be downloaded from https://github.com/synaptics-astra/usb-tool/tree/main/astra-usbboot-images. Custom build boot images are also supported. The tool expects boot images to be stored in the astra-usbboot-image directory located in the directory where the tool is executed from.

### Updating eMMC

The eMMC update image is a directory which contains image and partition information along with sub images which are sent to the board for flashing. Typically the directory is named ``eMMCimg`` for prebuilt images or ``SYNAIMG`` when building a custom image. If the ``--flash`` command line parameter is not provided, ``astra-update`` will flash the ``eMMCimg`` or ``SYNAIMG`` image located in the current directory.

Pre-built Astra SDK images can be found at https://github.com/synaptics-astra/sdk/releases

Flash the ``eMMCimg`` or ``SYNAIMG`` image stored in the current directory.

```bash
    astra-update
```

To specify the path of the update image:

```bash
    astra-update --flash /home/user/Downloads/eMMCimg
```

To specify the path of the boot image collection and update image:

```bash
    astra-update --boot-image-collection /home/user/Downloads/ \ 
        astra-usbboot-images --flash /home/user/custom_sl1680_image
```

If the update image does not contain a ``manifest.yaml`` file then the Boot Image ID can be specified on the command line:

```bash
    astra-update --boot-image-id da48b402-04e9-11f0-a78d-00155d2dc74c \
        -f /home/user/Downloads/eMMCimg
```

To have ``astra-update`` try to find the best boot image for a specific SoC and board:

```bash
    astra-update --chip sl1680 --board rdk --flash \
        /home/user/Downloads/eMMCimg
```

### Updating SPI

SPI update images can be a single file (.bin) or a directory containing the image and a ``manifest.yaml`` file. If no ``manifest.yaml`` file is provided then the required information can be provided on the command line. The pre-built SPI images provide ``manifest.yaml`` files and can be found at https://github.com/synaptics-astra/spi-u-boot/releases

To flash a SPI update image directory run:

```bash
    astra-update --flash /home/user/Downloads/sl1680
```

To flash SPI images without a ``manifest.yaml``:

```bash
    astra-update --image-type spi --chip sl1680 --flash /home/user/Downloads/spi_uboot_en.bin
```

### Additional Options

Astra Update also has additional command line parameters for providing details about images and debugging.

* -l, --log arg - path to store the log file. Use ``stdout`` to log to the console.
* -D, --debug - provides additional debug messages in the log file.
* -C, --continuous - will wait continuously for devices to connect. The default behavior is to exit after the first device completes.
* -T, --temp-dir arg - specify the path of the temp directory.
* -M, --manifest arg - specify the path to a ``manifest.yaml`` file.
* -u, --usb-debug - enable libusb debugging and output it to the console.
* -S, --simple-progress - print progress messages instead of using indicator progress bars. Better for logging.
* -p, port - Filter devices based on their port. USB devices from other ports will be ignored. Ports provided in a comma
    separated string (ie, "1-2,3-9").

These command line parameters describe the update image. If the image contains a ``manifest.yaml`` file then these parameters will override those in the file.

* -f, --flash arg - the path to the update image.
* -b, --board arg - the board required for this update image.
* -c, --chip arg - the SoC required for this update image.
* -i, --boot-image-id arg - the boot image ID required for this update image.
* -t, --image-type arg - the type of the update image (eMMC, SPI, NAND).
* -s, --secure-boot arg - the version of secure boot required for this update image.
* -m, --memory-layout arg - the memory layout of the update image.
* -r, --disable-reset - Do not reset the device after a successful update.

### Running on Windows

Astra Update requires a USB Kernel Driver to be installed on Windows. See https://synaptics-astra.github.io/doc/v/1.6.0/linux/index.html#installing-the-winusb-driver-windows-only

### Running on Linux

Astra Update provides a udev rules file which will set the permissions for the Astra Machina USB device to read / write for all. This allows non privileged processes to access Astra devices and allows ``astra-update`` to run without root permissions. Otherwise, ``astra-update`` should be run using ``sudo``.

The file is located in the ``config`` directory and can be installed by running:

```bash
    sudo cp config/99-astra-update.rules /etc/systemd/system
```

Then restart udev:

```bash
    sudo udevadm control --reload-rules
    sudo udevadm trigger
```

## Build

Astra Update uses ``cmake`` as the build system to support all three platforms.

### Dependencies

Building requires CMake version 3.10 or later.

Linux requires the libudev development package to be install. On Debian / Ubuntu based system run the command.

```bash
    sudo apt install libudev-dev
```

Building on Windows requires a minimum version of Visual Studio 2019 (SDK v142). Astra Update is using the C++17 standard which may not be
supported on older Platform Toolsets. 

Building on Mac OS requires the MacOS SDK to be installed. This is included when installing Xcode. The default version is MacOSX15.2.sdk, but this can be modified by change the ``include_directories`` line in ``cmake/CMakeLists.macos.txt``.

### Download the Source

```bash
    git clone https://github.com/aduggan-syna/astra-update.git
```

### Building the Source

Build the release version of tool and library using ``cmake``. 

```bash
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config release
```

Build a debug version using the debug build type.

```bash
    cmake -B build -DCMAKE_BUILD_TYPE=Debug
    cmake --build build --config debug
```

#### Building on Windows

On Windows, the first ``cmake`` command will generate Visual Studio Project files. The generated project and solution files will be located in the ``build`` directory.

Use the CMake Generator option to specify a specific version of Visual Studio.

```bash
    cmake -B build -DCMAKE_BUILD_TYPE=Release -G "Visual Studio 16 2019"
```

Next, open the solution file in Visual Studio or use the ``cmake --build`` command to build using the command line.

#### Building on Mac OS

Building on Mac OS allows the architecture to be set on the command line. If this option is not set the native architecture will be used.

For x86_64 based Macs:

```bash
    cmake -B build_x86_64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="x86_64"
    cmake --build build_x86_64 --config release
```

For Apple Silicon based Macs:

```bash
    cmake -B build_arm64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64"
    cmake --build build_arm64 --config release
```

> **Note:** The build system does not currently support building
Universal Binaries for Mac OS.

## Manifest Files

The ``manifest.yaml`` files are used to describe boot and update images and help Astra Update determine which boot image to use for a specific update image. 

The boot image contains the unique id, SoC and board information, memory layout, u-boot information, and the device vendor and product ids which the tool will connect to. Boot images require a ``manifest.yaml`` to know which devices to connect to and what feature are enable in this instance of U-Boot.

Example boot image ``manifest.yaml``:

```yaml
    id: 9930c714-375e-11f0-b558-0242ac110002
    chip: sl1680
    board: rdk
    secure_boot: genx
    vendor_id: 06CB
    product_id: 00B1
    console: uart
    uenv_support: true
    memory_layout: 4gb
    uboot: suboot
    uboot_version: "U-Boot 2019.10.202505221539_202505221945-g1581c16241-dirty (May 21 2025 - 15:54:17 +0000)"
```

The ``manifest.yaml`` file provided with an eMMC image specifies which boot image is required to flash the image. The ``boot_image`` is the ID which the tool will use to select the boot image. The other fields provide additional information about the update image. If no ``manifest.yaml`` is provide with the update image, then the tool can determine which boot image to use based on the command line parameters or the characteristics of the update image.

Example eMMCimg ``manifest.yaml``:

```yaml
    boot_image: 930c714-375e-11f0-b558-0242ac110002
    image_type: emmc
    chip: sl1680
    board: rdk
    memory_layout: 4gb
    reset: enable
```

The ``manifest.yaml`` file provided with a SPI image specifies which boot image is required to flash the image. The ``boot_image`` is the ID which the tool will use to select the boot image. The ``image_file`` parameter identifies the name of the image file, since SPI images to not have a specific naming convention. The other fields provide additional information about the update image. If no ``manifest.yaml`` is provide with the update image, then the tool can determine which boot image to use based on the command line parameters.

Example SPI ``manifest.yaml``:

```yaml
    boot_image: 9930c714-375e-11f0-b558-0242ac110002
    image_type: spi
    chip: sl1680
    board: rdk
    image_file: u-boot-astra-v1.1.1.sl1680.rdk.spi.bin
    reset: enable
```
SPI ``manifest.yaml`` files also support listing multiple SPI images in maps. This is useful for flashing multiple SPI images at once. The ``images`` map contains
a section per file. The section title is the file name and subsequent properties are the properties used for writing the file to the SPI flash.

Example SPI ``manifest.yaml`` using image maps:

```yaml
    boot_image: 9930c714-375e-11f0-b558-0242ac110002
    image_type: spi
    chip: sl1680
    board: rdk
    reset: enable

    images:
        u-boot-astra-v1.1.1.sl1680.rdk.spi.bin:
```
Example SPI ``manifest.yaml`` for Writing a Linux boot image:

```yaml
    boot_image: 9930c714-375e-11f0-b558-0242ac110002
    image_type: spi
    chip: sl1680
    board: rdk
    reset: enable

    images:
        spi_suboot.bin:
            read_address: 0x10000000
            write_first_copy_address: 0xf0000000
            write_second_copy_address: 0xf0200000
            write_length: 0x1f0000
            erase_first_start_address: 0xf0000000
            erase_first_length: 0xf01fffff
            erase_second_start_address: 0xf0200000
            erase_second_length: 0xf03fffff

        boot.subimg:
            read_address: 0x10000000
            write_first_copy_address: 0xf0400000
            write_second_copy_address: 0xf1200000
            write_length: 0xd0c990
            erase_first_start_address: 0xf0400000
            erase_first_length: 0xf11fffff
            erase_second_start_address: 0xf1200000
            erase_second_length: 0xf1ffffff
```
