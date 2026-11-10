# Python Bindings Testing Guide

## Overview

The test suite for Python bindings is organized into:

1. **Binding Tests** (`test_bindings.py`) - Verify C++ module integration
2. **Response Handler Tests** (`test_response_handler.py`) - Verify event handling
3. **Image Loader Tests** (in `test_response_handler.py`) - Verify image loading
4. **Integration Tests** (planned) - End-to-end device operations

## Running Tests

### Prerequisites

```bash
pip install pytest pytest-mock
```

### Run All Tests

```bash
cd /path/to/astra-update
python -m pytest tests/ -v
```

### Run Specific Test File

```bash
# Test C++ bindings
python -m pytest tests/test_bindings.py -v

# Test response handling
python -m pytest tests/test_response_handler.py -v
```

### Run Specific Test Class

```bash
python -m pytest tests/test_bindings.py::TestBindingsImport -v
```

### Run Specific Test

```bash
python -m pytest tests/test_bindings.py::TestBindingsImport::test_import_main_module -v
```

### Skip Missing Module Tests

If the C++ module isn't built, tests automatically skip:

```bash
python -m pytest tests/ -v
# Tests will be marked as SKIPPED if astra_update module isn't available
```

### Run with Coverage

```bash
pip install pytest-cov
python -m pytest tests/ --cov=python/astra_update --cov-report=html
# Open htmlcov/index.html for results
```

## Test Structure

### Test Categories

#### 1. Binding Tests (`test_bindings.py`)

Tests that verify the C++ module can be imported and used:

- **Import Tests**: Verify module, classes, and enums can be imported
- **Instantiation Tests**: Create objects with default and custom arguments
- **Method Tests**: Call methods and check return values
- **Enum Tests**: Verify all enum values are accessible
- **Current API parity**: Compare device-status and log-level enums and the
  runtime version against native headers; verify XSPI `True`/`False`/`None`
  metadata, series-specific flash completion commands, wait-event forwarding,
  and connection-open failure handling in the boot CLI.

**Status**: ✓ Can run without real hardware (mocked callbacks)

**Example Test**:
```python
def test_import_main_module(self):
    from astra_update import AstraDeviceManager, __version__
    # Verify it's the real C++ module, not a stub
    version = AstraDeviceManager.get_version()
    assert version == __version__
```

#### 2. Response Handler Tests (`test_response_handler.py`)

Tests for Python-level response handling:

- **Event Tests**: Create and type Event objects
- **Callback Tests**: Register and invoke callbacks
- **Polling Tests**: Poll responses with timeouts
- **Waiting Tests**: Wait for specific statuses

**Status**: ✓ Can run with mocked device manager (no real hardware needed)

**Example Test**:
```python
def test_callback_registration(self):
    mock_mgr = MagicMock()
    handler = ResponseHandler(mock_mgr)
    
    callback = Mock()
    handler.register_callback(EventType.DEVICE_EVENT, callback)
    
    assert callback in handler._callbacks[EventType.DEVICE_EVENT]
```

#### 3. Image Loader Tests

Tests for image file handling:

- **File Loading**: Load existing files
- **Error Handling**: Handle missing/invalid files
- **File Validation**: Verify path resolution

**Status**: ✓ Can run without real images (mocked or test files)

**Example Test**:
```python
def test_load_flash_image_file_not_found(self):
    with pytest.raises(FileNotFoundError):
        load_flash_image("/nonexistent/path/image.bin")
```

#### 4. Integration Tests (Planned)

Tests with real devices:

- **Boot Operations**: Actually boot real Astra devices
- **Update Operations**: Actually update real devices
- **Event Flow**: Verify complete event sequences
- **Error Recovery**: Test error handling with real hardware

**Status**: ⏳ Planned - requires real Astra devices

**Would Mark As**:
```python
@pytest.mark.integration
def test_boot_device_real_hardware(self):
    # Requires connected Astra device
    pass
```

## Test Execution Flow

### Without Compiled Module

1. Tests import pytest and mocking libraries
2. Try to import `astra_update` module
3. If import fails:
   - Binding tests are skipped
   - Python-level tests still run with mocks
4. All tests that don't require real hardware still execute

### With Compiled Module (After Build)

1. C++ module is successfully imported
2. Real objects can be created
3. All tests execute with real bindings
4. No need for mocks in binding tests

### With Real Hardware

1. All unit tests pass
2. Integration tests can be enabled with `-m integration`
3. Real devices are detected and used

## Test Coverage

Current test coverage targets:

```
python/astra_update/
  __init__.py           - 100% (import statements)
  response_handler.py   - 85%+ (main paths tested, error paths)
  images.py             - 90%+ (file handling, error cases)

lib/
  astra_update_py.cpp   - Tested via Python tests
                          (binding exposure verified)
```

To check coverage:

```bash
python -m pytest tests/ --cov=python/astra_update --cov-report=term-missing
```

## Adding New Tests

### Test File Naming

- Test files: `tests/test_<module>.py`
- Conftest: `tests/conftest.py`

### Test Class Naming

```python
class Test<FeatureName>:
    """Test <FeatureName> functionality."""
    
    def test_<specific_behavior>(self):
        """Test description."""
```

### Test Markers

```python
@pytest.mark.integration  # Requires real hardware
@pytest.mark.slow         # Takes > 5 seconds
```

### Example Test

```python
def test_response_handler_callback_called(self, mock_device_manager):
    """Verify callbacks are invoked."""
    from astra_update.response_handler import ResponseHandler, EventType
    
    handler = ResponseHandler(mock_device_manager)
    callback = Mock()
    handler.register_callback(EventType.DEVICE_EVENT, callback)
    
    # Simulate event (mocked)
    event = Mock()
    event.event_type = EventType.DEVICE_EVENT
    
    handler._invoke_callbacks(event)
    
    callback.assert_called_once_with(event)
```

## Debugging Tests

### Run with Detailed Output

```bash
# Show print statements and full tracebacks
python -m pytest tests/ -vv -s

# Stop on first failure
python -m pytest tests/ -x

# Enter pdb on failures
python -m pytest tests/ --pdb

# Show local variables on failure
python -m pytest tests/ -l
```

### Run Single Test

```bash
python -m pytest tests/test_bindings.py::TestDeviceManagerInstantiation::test_create_device_manager_defaults -vv -s
```

### Profile Tests

```bash
pip install pytest-benchmark
python -m pytest tests/ --benchmark-only
```

## Known Issues & Workarounds

### Issue: "No module named 'astra_update'"

**Solution**: Build the C++ module first
```bash
cmake -S . -B build_py -DCMAKE_BUILD_TYPE=Release -DASTRA_BUILD_PYTHON_BINDINGS=ON
cmake --build build_py --config Release --target _astra_update
```

Then ensure it's in Python path:
```bash
export PYTHONPATH=/path/to/astra-update/python:$PYTHONPATH
python -m pytest tests/
```

### Issue: Tests timeout (10+ seconds)

**Solution**: This is expected for waiting tests with timeouts
```bash
# Run without slow tests
python -m pytest tests/ -m "not slow"

# Or set a custom timeout
python -m pytest tests/ --timeout=30
```

### Issue: Mock objects not behaving as expected

**Solution**: Use `from unittest.mock import MagicMock` instead of `Mock()`
```python
# Better - handles attribute access
mock = MagicMock()
mock.some_method().return_value = x

# Less flexible
mock = Mock()
mock.some_method = Mock(return_value=x)
```

## Continuous Integration

Tests can be run in CI/CD pipelines:

```bash
# GitHub Actions example
- name: Run Tests
  run: |
    pip install pytest pytest-cov
    python -m pytest tests/ --cov=python/astra_update --cov-report=xml
    
- name: Upload Coverage
  uses: codecov/codecov-action@v3
  with:
    files: ./coverage.xml
```

## Benchmarking

To profile test performance:

```bash
python -m pytest tests/ --profile
```

Or with pytest-benchmark:

```bash
pip install pytest-benchmark
python -m pytest tests/ --benchmark-only
```

## Future Testing Plans

1. **Integration Tests**: Real device operations
2. **Performance Tests**: Boot/update speed benchmarks
3. **Concurrency Tests**: Multiple device handling
4. **Stress Tests**: Rapid boot/update cycles
5. **Error Path Tests**: Failure modes and recovery
