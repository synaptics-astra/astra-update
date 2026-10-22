# Python Bindings for Astra Update

The `python/` directory contains the Python package and CLI launcher for the astra-update library.

## Structure

- `python/astra_update/` - Python package with:
  - `__init__.py` - Main package exports (C++ bindings + Python utilities)
  - `response_handler.py` - High-level response handling with events and callbacks
  - `images.py` - Convenience functions for loading flash/boot images
    - `boot.py` - Importable implementation of the boot CLI
- `python/astra-boot.py` - Launcher for the Python `astra-boot` CLI tool
- `tests/` - Test suite

## Building the Python Module

### Prerequisites

You must have the following installed:
- Python 3.8+ (with development headers)
- CMake 3.18+
- C++ compiler (MSVC on Windows, GCC/Clang on Linux/macOS)
- pip

### Build Steps

1. Navigate to the project root:
   ```bash
   cd /path/to/astra-update
   ```

2. Configure the build directory from the project root:
   ```bash
   cmake -S . -B build_py -DCMAKE_BUILD_TYPE=Release -DASTRA_BUILD_PYTHON_BINDINGS=ON
   ```

3. Build the Python module:
   ```bash
   cmake --build build_py --config Release --target _astra_update
   ```

4. During the build, the extension module is copied into `python/astra_update/` as:
    - Windows: `_astra_update*.pyd`
    - Linux/macOS: `_astra_update*.so`

Python bindings are disabled by default for native-only builds. The extension
uses pybind11 3.0.2 and must be built with the same Python interpreter used to
run it. Pass `-DPython_EXECUTABLE=/path/to/python` to select an interpreter.

### Installation

Run these commands from the repository root, using the same Python environment
for both the CMake build and pip:

```bash
python -m pip install -e .
```

The root `pyproject.toml` configures setuptools to install the package from
`python/astra_update/` in editable mode. Build the native extension first using
the steps above: `pip install -e .` installs the Python package and CLI entry
point, but does not compile the C++ extension.

After installation, Python code can import `astra_update`, and the CLI is
available as `astra-boot-py`. To run the script directly without installing:

```bash
python python/astra-boot.py <images>
```

## Usage

### Command-Line Tool

The Python version of `astra-boot` uses the same library as the C++ version:

```bash
python python/astra-boot.py <boot-images-path> [options]
```

**Options:**
- `-h, --help` - Show help message
- `-v, --version` - Show version
- `-D, --debug` - Enable debug logging
- `-C, --continuous` - Update multiple devices continuously
- `-T, --temp-dir` - Temporary directory
- `-u, --usb-debug` - Enable USB debug logging
- `-S, --simple-progress` - Disable progress bars, use simple output
- `-o, --boot-command` - Boot command
- `-p, --port` - Filter by USB port
- `-e, --exit-on-error` - Exit on first error in continuous mode
- `-b, --boot-stage` - Target boot stage: auto, bootloader, linux, m52bl, sysmgr
- `-k, --keep-image-request-loop` - Keep serving image requests after boot completes
- `--leave-fastboot-idle` - Leave an SL26XX device idle in fastboot
- `-l, --log` - Log file path
- `-M, --manifest` - Manifest file path

**Example:**
```bash
python python/astra-boot.py ~/astra-usbboot-images --boot-stage auto --continuous
```

### Python API

#### Basic Usage

```python
from astra_update import AstraDeviceManager, DeviceBootStage, LogLevel
from astra_update.response_handler import ResponseHandler, EventType

# Create device manager
mgr = AstraDeviceManager(
    continuous=False,
    log_level=LogLevel.INFO,
    keep_image_request_loop_after_boot=False,
    leave_fastboot_idle=False,
)

# Create response handler for convenient event processing
handler = ResponseHandler(mgr)

# Start boot operation
mgr.boot("/path/to/boot/images", boot_stage=DeviceBootStage.AUTO)

# Process responses
while True:
    event = handler.get_next_response(timeout=1.0)
    if event is None:
        break
    
    if event.is_device_event():
        device_response = event.get_device_response()
        print(f"Device {device_response.device_name}: {device_response.status}")

# Shutdown
mgr.shutdown()
```

#### Image Loading

```python
from astra_update.images import load_flash_image, load_spi_flash_image

# Load flash image with optional config overrides and manifest path
flash_image = load_flash_image(
    image_path="firmware.bin",
    config={"board": "sl1680", "chip": "sl1680", "image_type": "emmc"},
    manifest="/path/to/manifest.yaml"
)

# Update devices
mgr.update(flash_image, "/path/to/boot/images")
```

`load_flash_image()` calls the native `Load()` method and raises `RuntimeError`
with the library's load diagnostic on failure. When using
`FlashImage.flash_image_factory()` directly, call `load()` before `mgr.update()`.
Flash images expose metadata getters, `get_load_error()`, and U-Boot version
selection through `UbootVersion` and `set_uboot_version()`.

#### Response Handling with Callbacks

```python
from astra_update.response_handler import ResponseHandler, EventType

handler = ResponseHandler(mgr)

def on_device_event(event):
    print(f"Device event: {event.get_device_response()}")

def on_manager_event(event):
    print(f"Manager event: {event.get_manager_response()}")

handler.register_callback(EventType.DEVICE_EVENT, on_device_event)
handler.register_callback(EventType.MANAGER_EVENT, on_manager_event)

# Process all responses with callbacks
mgr.boot(...)
while handler.has_responses() or mgr.response_queue_size() > 0:
    handler.poll_responses()
    time.sleep(0.01)
```

#### Waiting for Specific Status

```python
from astra_update import DeviceStatus

# Wait for device to complete boot
success = handler.wait_for_status(
    device_name="Device1",
    target_status=DeviceStatus.BOOT_COMPLETE,
    timeout=60.0  # seconds
)

if success:
    print("Boot completed!")
else:
    print("Boot timeout!")
```

## Python Bindings Reference

### Classes

#### `AstraDeviceManager`

Main interface to the astra-update library.

**Constructor:**
```python
AstraDeviceManager(
    continuous: bool = False,
    log_level: LogLevel = LogLevel.WARNING,
    log_path: str = "",
    temp_dir: str = "",
    filter_ports: str = "",
    usb_debug: bool = False
)
```

**Methods:**
- `boot(boot_images_path, boot_command="", boot_stage=AUTO)` - Boot devices
- `update(flash_image, boot_image_path)` - Update devices
- `shutdown() -> bool` - Shutdown and return error status
- `get_log_file() -> str` - Get log file path
- `get_response() -> DeviceManagerResponse | None` - Get next response (non-blocking)
- `get_responses() -> list[DeviceManagerResponse]` - Get all pending responses
- `has_responses() -> bool` - Check if responses pending
- `response_queue_size() -> int` - Get queue size
- `get_version()` - Get library version (static)

#### `ResponseHandler`

High-level response processing with event callbacks.

**Methods:**
- `poll_responses() -> list[Event]` - Get all pending responses
- `get_next_response(timeout=None) -> Event | None` - Wait for next response
- `register_callback(event_type, callback)` - Add event handler
- `unregister_callback(event_type, callback)` - Remove event handler
- `wait_for_status(device_name, status, timeout=None) -> bool` - Wait for device status
- `wait_for_manager_shutdown(timeout=None) -> bool` - Wait for shutdown
- `has_responses() -> bool` - Check for pending responses
- `drain_responses() -> list[Event]` - Get and clear all responses

### Enums

- `LogLevel` - DEBUG, INFO, WARNING, ERROR
- `DeviceBootStage` - AUTO, BOOTLOADER, LINUX, M52BL, SYSMGR
- `DeviceStatus` - ADDED, OPENED, CLOSED, BOOT_START, BOOT_PROGRESS, BOOT_COMPLETE, BOOT_FAIL, UPDATE_START, UPDATE_PROGRESS, UPDATE_COMPLETE, UPDATE_FAIL, IMAGE_SEND_START, IMAGE_SEND_PROGRESS, IMAGE_SEND_COMPLETE, IMAGE_SEND_FAIL
- `DeviceManagerStatus` - START, INFO, FAILURE, SHUTDOWN
- `DeviceSeries` - SL16XX, SL26XX, SR1XX

### Data Classes

#### `DeviceResponse`

Response data for device events.

**Attributes:**
- `device_name: str` - Name/ID of the device
- `status: DeviceStatus` - Current status
- `progress: float` - Progress percentage (0-100)
- `image_name: str` - Associated image name
- `message: str` - Status message

#### `ManagerResponse`

Response data for manager events.

**Attributes:**
- `manager_status: DeviceManagerStatus` - Manager status
- `manager_message: str` - Status message

#### `Event`

Wrapped response with metadata.

**Attributes:**
- `event_type: EventType` - MANAGER_EVENT or DEVICE_EVENT
- `timestamp: float` - Unix timestamp (seconds)
- `manager_response` - Original response object

**Methods:**
- `is_manager_event() -> bool`
- `is_device_event() -> bool`
- `get_manager_response() -> ManagerResponse | None`
- `get_device_response() -> DeviceResponse | None`

## Testing

Run the test suite:

```bash
python -m pytest tests/ -v
```

Individual test files:
```bash
python -m pytest tests/test_bindings.py -v         # C++ binding tests
python -m pytest tests/test_response_handler.py -v # Response handling tests
python -m pytest tests/test_images.py -v           # Image loading tests
python -m pytest tests/test_integration.py -v      # Full integration tests
```

## Architecture

### Design Decisions

1. **pybind11 for Bindings**: Chosen for excellent C++ support, header-only library, and native handling of C++17 variants and enums

2. **Queue-based Response Handling**: Instead of asyncio, uses a thread-safe queue that mirrors the C++ event loop pattern in `astra-boot.cpp`. Python threads block on `queue.get()` while C++ callbacks enqueue responses.

3. **Exact CLI Match**: The Python `astra-boot` tool uses identical command-line arguments and output formatting as the C++ version for drop-in compatibility.

4. **ResponseHandler Abstraction**: Provides callback registration, event typing, and convenience methods (like `wait_for_status`) without requiring lower-level queue management.

### Threading Model

- **C++ Callbacks**: Called from AstraDeviceManager threads via pybind11
- **Response Queue**: Thread-safe queue (std::mutex protected) bridges C++ and Python
- **Python Polling**: ResponseHandler polls queue with configurable interval (default: 10ms)
- **No GIL Issues**: Callback puts into queue and returns immediately; Python polls non-blocking

## Troubleshooting

### Import Error: "No module named 'astra_update'"

The Python module isn't built yet. Run build steps above, then ensure:
```bash
export PYTHONPATH=/path/to/astra-update/python:$PYTHONPATH
```

### Permission Denied on USB Device

You need appropriate permissions:
```bash
# Linux
sudo usermod -a -G plugdev $USER

# Or use udev rules (see config/99-astra-update.rules)
sudo cp config/99-astra-update.rules /etc/udev/rules.d/
sudo udevadm control --reload  
```

### Build Failures

Ensure:
- Python development headers: `python3-dev` (Linux) or Python installation with development headers
- CMake 3.18+ with pybind11 support
- OpenSSL development libraries if needed

Run with verbose output:
```bash
cmake --build . --config Release --verbose
```

## Contributing

When modifying the Python bindings:

1. Update C++ pybind11 module in `lib/astra_update_py.cpp`
2. Update Python wrapper in `python/astra_update/` package files
3. Add tests in `tests/`
4. Ensure exact behavior match with C++ version
5. Test on both Windows and Linux

## License

Apache License 2.0 - See LICENSE file
