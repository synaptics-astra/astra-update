"""
Tests for C++ bindings through pybind11.

These tests verify that:
1. The C++ module can be imported
2. All classes and enums are accessible
3. Basic object creation works
4. C++ methods are callable from Python
"""

import pytest
import sys
from pathlib import Path

# Add src to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent / "src"))


class TestBindingsImport:
    """Test that bindings can be imported."""

    def test_import_main_module(self):
        """Test importing the astra_update C++ module."""
        try:
            import astra_update
            assert hasattr(astra_update, 'AstraDeviceManager')
        except ImportError as e:
            pytest.skip(f"C++ module not built: {e}")

    def test_import_enums(self):
        """Test that all enums are accessible."""
        try:
            from astra_update import (
                LogLevel,
                DeviceBootStage,
                DeviceStatus,
                DeviceManagerStatus,
                DeviceSeries,
            )
            # Verify enum values exist
            assert LogLevel.DEBUG
            assert DeviceBootStage.AUTO
            assert DeviceStatus.ADDED
            assert DeviceManagerStatus.START
            assert DeviceSeries.SL16XX
        except ImportError as e:
            pytest.skip(f"Bindings not available: {e}")

    def test_import_classes(self):
        """Test that all classes are accessible."""
        try:
            from astra_update import (
                AstraDeviceManager,
                DeviceManagerResponse,
                DeviceResponse,
                ManagerResponse,
                AstraDevice,
                FlashImage,
                ResponseQueue,
            )
            # Verify classes can be referenced
            assert AstraDeviceManager is not None
            assert DeviceManagerResponse is not None
        except ImportError as e:
            pytest.skip(f"Bindings not available: {e}")


class TestDeviceManagerInstantiation:
    """Test AstraDeviceManager instantiation."""

    def test_create_device_manager_defaults(self):
        """Test creating device manager with default arguments."""
        try:
            from astra_update import AstraDeviceManager, LogLevel
            
            mgr = AstraDeviceManager()
            assert mgr is not None
        except ImportError as e:
            pytest.skip(f"Bindings not available: {e}")

    def test_create_device_manager_with_options(self):
        """Test creating device manager with options."""
        try:
            from astra_update import AstraDeviceManager, LogLevel
            
            mgr = AstraDeviceManager(
                continuous=False,
                log_level=LogLevel.DEBUG,
                log_path="",
                temp_dir="",
                filter_ports="",
                usb_debug=False
            )
            assert mgr is not None
        except ImportError as e:
            pytest.skip(f"Bindings not available: {e}")

    def test_get_version(self):
        """Test getting library version."""
        try:
            from astra_update import AstraDeviceManager
            
            version = AstraDeviceManager.get_version()
            assert isinstance(version, str)
            assert len(version) > 0
            # Should be semver-like
            assert version[0].isdigit()
        except ImportError as e:
            pytest.skip(f"Bindings not available: {e}")


class TestResponseStructures:
    """Test response data structures."""

    def test_create_device_response(self):
        """Test creating DeviceResponse."""
        try:
            from astra_update import DeviceResponse, DeviceStatus
            
            resp = DeviceResponse()
            resp.device_name = "TestDevice"
            resp.status = DeviceStatus.ADDED
            resp.progress = 50.0
            resp.image_name = "test.bin"
            
            assert resp.device_name == "TestDevice"
            assert resp.status == DeviceStatus.ADDED
            assert resp.progress == 50.0
        except ImportError as e:
            pytest.skip(f"Bindings not available: {e}")

    def test_create_manager_response(self):
        """Test creating ManagerResponse."""
        try:
            from astra_update import ManagerResponse, DeviceManagerStatus
            
            resp = ManagerResponse()
            resp.manager_status = DeviceManagerStatus.INFO
            resp.manager_message = "Test message"
            
            assert resp.manager_status == DeviceManagerStatus.INFO
            assert resp.manager_message == "Test message"
        except ImportError as e:
            pytest.skip(f"Bindings not available: {e}")

    def test_create_manager_response_from_variant(self):
        """Test creating DeviceManagerResponse from ManagerResponse."""
        try:
            from astra_update import (
                DeviceManagerResponse,
                ManagerResponse,
                DeviceManagerStatus
            )
            
            mgr_resp = ManagerResponse()
            mgr_resp.manager_status = DeviceManagerStatus.INFO
            
            dv_resp = DeviceManagerResponse(mgr_resp)
            assert dv_resp.is_device_manager_response()
            assert not dv_resp.is_device_response()
        except ImportError as e:
            pytest.skip(f"Bindings not available: {e}")


class TestStaticMethods:
    """Test static methods on classes."""

    def test_astra_device_boot_stage_from_string(self):
        """Test AstraDevice.boot_stage_from_string()."""
        try:
            from astra_update import AstraDevice, DeviceBootStage
            
            stage = AstraDevice.boot_stage_from_string("auto")
            assert stage == DeviceBootStage.AUTO
            
            stage = AstraDevice.boot_stage_from_string("linux")
            assert stage == DeviceBootStage.LINUX
        except ImportError as e:
            pytest.skip(f"Bindings not available: {e}")

    def test_astra_device_status_to_string(self):
        """Test AstraDevice.status_to_string()."""
        try:
            from astra_update import AstraDevice, DeviceStatus
            
            status_str = AstraDevice.status_to_string(DeviceStatus.ADDED)
            assert isinstance(status_str, str)
            assert len(status_str) > 0
        except ImportError as e:
            pytest.skip(f"Bindings not available: {e}")

    def test_astra_device_series_to_string(self):
        """Test AstraDevice.series_to_string()."""
        try:
            from astra_update import AstraDevice, DeviceSeries
            
            series_str = AstraDevice.series_to_string(DeviceSeries.SL16XX)
            assert isinstance(series_str, str)
            assert len(series_str) > 0
        except ImportError as e:
            pytest.skip(f"Bindings not available: {e}")


class TestResponseQueue:
    """Test ResponseQueue access."""

    def test_get_response_queue_instance(self):
        """Test getting ResponseQueue singleton."""
        try:
            from astra_update import ResponseQueue
            
            queue = ResponseQueue.get_instance()
            assert queue is not None
        except ImportError as e:
            pytest.skip(f"Bindings not available: {e}")

    def test_response_queue_empty(self):
        """Test that queue starts empty."""
        try:
            from astra_update import ResponseQueue
            
            queue = ResponseQueue.get_instance()
            assert queue.is_empty()
            assert queue.size() == 0
        except ImportError as e:
            pytest.skip(f"Bindings not available: {e}")


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
