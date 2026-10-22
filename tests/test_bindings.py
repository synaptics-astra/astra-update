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
        from astra_update import (
            FlashImage,
            FlashImageType,
            ImageType,
            MemoryDDRType,
            MemoryLayout,
            SecureBootVersion,
            UbootVersion,
        )

        image_path = tmp_path / "firmware.bin"
        image_path.write_bytes(b"firmware")
        (tmp_path / "manifest.yaml").write_text(
            "chip: sl1680\n"
            "image_type: spi\n"
            "secure_boot: gen2\n"
            "memory_layout: 4gb\n"
            "ddr_type: lpddr4x\n"
            "images:\n  firmware.bin: {}\n",
            encoding="utf-8",
        )
        image = FlashImage.flash_image_factory(
            str(tmp_path), {"chip": "sl1680", "image_type": "spi"}
        )
        assert image.get_chip_name() == "sl1680"
        assert image.get_flash_image_type() == FlashImageType.SPI
        assert image.get_secure_boot_version() == SecureBootVersion.GEN2
        assert image.get_memory_layout() == MemoryLayout.GB_4
        assert image.get_memory_ddr_type() == MemoryDDRType.LPDDR4X
        assert image.get_reset_when_complete() is True
        image.set_uboot_version(UbootVersion.V2025_01)
        assert image.get_uboot_version() == UbootVersion.V2025_01
        assert image.load() == 0
        assert image.get_load_error() == ""
        assert image.get_final_image() == "firmware.bin"
        flash_images = image.get_images()
        assert len(flash_images) == 1
        flash_image = flash_images[0]
        assert flash_image.get_name() == "firmware.bin"
        assert Path(flash_image.get_path()) == image_path
        assert flash_image.get_image_type() == ImageType.UPDATE_SPI
        assert flash_image.load() == 0
        assert flash_image.get_size() == len(b"firmware")

    def test_native_boot_image_getters(self, tmp_path):
        from astra_update import (
            AstraBootImage,
            DeviceBootStage,
            MemoryDDRType,
            MemoryLayout,
            SecureBootVersion,
            TransportType,
            UbootConsole,
            UbootVariant,
            UbootVersion,
        )

        (tmp_path / "manifest.yaml").write_text(
            "id: test-boot\n"
            "chip: sl1680\n"
            "board: test-board\n"
            "console: uart\n"
            "uenv_support: true\n"
            "vendor_id: '1234'\n"
            "product_id: '5678'\n"
            "secure_boot: gen2\n"
            "memory_layout: 4gb\n"
            "ddr_type: ddr4\n"
            "uboot: uboot\n"
            "uboot_version: '2025.01'\n",
            encoding="utf-8",
        )
        (tmp_path / "bl.bin").write_bytes(b"boot")

        boot_image = AstraBootImage(str(tmp_path))
        assert boot_image.load()
        assert boot_image.get_vendor_id() == 0x1234
        assert boot_image.get_product_id() == 0x5678
        assert boot_image.get_vendor_product_id_pairs() == [(0x1234, 0x5678)]
        assert boot_image.get_sysmgr_vendor_id() == 0
        assert boot_image.get_sysmgr_product_id() == 0
        assert boot_image.get_fastboot_vendor_id() == 0
        assert boot_image.get_fastboot_product_id() == 0
        assert boot_image.get_chip_name() == "sl1680"
        assert boot_image.get_board_name() == "test-board"
        assert boot_image.get_id() == "test-boot"
        assert boot_image.get_uenv_support() is True
        assert boot_image.get_nand_support() is False
        assert boot_image.get_secure_boot_version() == SecureBootVersion.GEN2
        assert boot_image.get_uboot_console() == UbootConsole.UART
        assert boot_image.get_memory_layout() == MemoryLayout.GB_4
        assert boot_image.get_memory_ddr_type() == MemoryDDRType.DDR4
        assert boot_image.get_transport_type() == TransportType.USB
        assert boot_image.get_uboot_variant() == UbootVariant.UBOOT
        assert boot_image.get_uboot_version() == UbootVersion.V2025_01
        assert boot_image.get_final_boot_image() == "minildr.img"
        assert boot_image.is_linux_boot() is False
        assert boot_image.get_default_boot_stage() == DeviceBootStage.AUTO
        assert len(boot_image.get_images()) == 1


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
