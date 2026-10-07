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

# Add python to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent / "python"))


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

    def test_current_manager_options(self):
        from astra_update import AstraDeviceManager

        mgr = AstraDeviceManager(
            keep_image_request_loop_after_boot=True,
            leave_fastboot_idle=True,
        )
        assert mgr.get_version() == "2.0.4"
        assert mgr.shutdown() is False

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


class TestBootTool:
    def test_current_options_are_forwarded(self, monkeypatch):
        from unittest.mock import Mock
        import astra_update.boot as astra_update_boot

        factory = Mock()
        factory.return_value.boot.return_value = 0
        monkeypatch.setattr(astra_update_boot, "AstraBootTool", factory)
        monkeypatch.setattr(sys, "argv", [
            "astra-boot-py", "images", "--keep-image-request-loop", "--leave-fastboot-idle"
        ])

        assert astra_update_boot.main() == 0
        assert factory.call_args.kwargs["keep_image_request_loop_after_boot"] is True
        assert factory.call_args.kwargs["leave_fastboot_idle"] is True

    def test_version_without_image_path(self, monkeypatch, capsys):
        import astra_update.boot as astra_update_boot
        from astra_update import __version__

        monkeypatch.setattr(sys, "argv", ["astra-boot-py", "--version"])
        assert astra_update_boot.main() == 0
        assert f"v{__version__}" in capsys.readouterr().out

    def test_shutdown_event_ends_loop(self, monkeypatch):
        from unittest.mock import Mock
        import astra_update.boot as astra_update_boot
        from astra_update import ManagerResponse, DeviceManagerStatus
        from astra_update.response_handler import Event, EventType

        monkeypatch.setattr(astra_update_boot, "AstraDeviceManager", Mock())
        tool = astra_update_boot.AstraBootTool("images", debug=True)
        response = ManagerResponse()
        response.manager_status = DeviceManagerStatus.SHUTDOWN
        event = Event(event_type=EventType.MANAGER_EVENT, timestamp=0.0, manager_response=response)

        tool._handle_manager_event(event)
        assert tool._should_shutdown()
        assert tool.usb_debug is False


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


class TestFlashImageLoading:
    def test_load_calls_native_load(self, tmp_path, monkeypatch):
        from unittest.mock import Mock
        from astra_update import images

        image = Mock()
        image.load.return_value = 0
        factory = Mock(return_value=image)
        monkeypatch.setattr(images.FlashImage, "flash_image_factory", factory)

        assert images.load_flash_image(str(tmp_path)) is image
        image.load.assert_called_once_with()

    def test_load_reports_native_error(self, tmp_path, monkeypatch):
        from unittest.mock import Mock
        from astra_update import images

        image = Mock()
        image.load.return_value = -1
        image.get_load_error.return_value = "Missing partition image"
        monkeypatch.setattr(images.FlashImage, "flash_image_factory", Mock(return_value=image))

        with pytest.raises(RuntimeError, match="Missing partition image"):
            images.load_flash_image(str(tmp_path))

    def test_native_image_api(self, tmp_path):
        from astra_update import FlashImage, FlashImageType, UbootVersion

        (tmp_path / "manifest.yaml").write_text(
            "chip: sl1680\nimage_type: spi\nimages:\n  missing.bin: {}\n",
            encoding="utf-8",
        )
        image = FlashImage.flash_image_factory(
            str(tmp_path), {"chip": "sl1680", "image_type": "spi"}
        )
        assert image.get_chip_name() == "sl1680"
        assert image.get_flash_image_type() == FlashImageType.SPI
        image.set_uboot_version(UbootVersion.V2025_01)
        assert image.get_uboot_version() == UbootVersion.V2025_01
        assert image.load() != 0
        assert "SPI image file not found" in image.get_load_error()


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
