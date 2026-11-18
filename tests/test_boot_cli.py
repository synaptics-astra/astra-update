"""
Tests for the Python astra-boot CLI (astra_update.boot) and the typed
flash-image loaders.
"""

import sys
from pathlib import Path
from unittest.mock import Mock

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "python"))


def _device_event(device_name, status_name, image_name="", message="", progress=0):
    from astra_update import DeviceManagerResponse, DeviceResponse, DeviceStatus
    from astra_update.response_handler import Event

    response = DeviceResponse()
    response.device_name = device_name
    response.status = getattr(DeviceStatus, status_name)
    response.progress = progress
    response.image_name = image_name
    response.message = message
    return Event.from_response(DeviceManagerResponse(response))


@pytest.fixture
def boot_tool(tmp_path):
    from astra_update.boot import AstraBootTool

    tool = AstraBootTool(str(tmp_path), temp_dir=str(tmp_path / "work"))
    yield tool
    tool.device_manager.shutdown()


class TestImageSendFail:
    def test_image_send_fail_is_reported_and_closes_its_progress_bar(self, boot_tool, capsys):
        from astra_update.boot import ProgressBar

        bar = Mock()
        other_bar = Mock()
        boot_tool.progress_bars["dev:spk.bin"] = ProgressBar("dev", "spk.bin", bar)
        boot_tool.progress_bars["dev:key.bin"] = ProgressBar("dev", "key.bin", other_bar)

        boot_tool._handle_device_event(
            _device_event("dev", "IMAGE_SEND_FAIL", "spk.bin", "Upload write failed"))

        bar.close.assert_called_once_with()
        other_bar.close.assert_not_called()
        assert list(boot_tool.progress_bars) == ["dev:key.bin"]
        err = capsys.readouterr().err
        assert "spk.bin" in err and "dev" in err and "Upload write failed" in err
        # Not fatal by itself: a fatal failure is followed by BOOT_FAIL.
        assert boot_tool.running

    def test_image_send_fail_without_a_progress_bar_is_still_reported(self, boot_tool, capsys):
        boot_tool._handle_device_event(
            _device_event("dev", "IMAGE_SEND_FAIL", "m52bl.bin", "Failed to open image"))

        assert "Failed to open image" in capsys.readouterr().err
        assert boot_tool.progress_bars == {}


class TestManifestOption:
    def test_manifest_is_accepted_but_warned_about(self, tmp_path, monkeypatch, capsys):
        import astra_update.boot as boot

        monkeypatch.setattr(boot.signal, "signal", lambda *args: None)
        missing = tmp_path / "no-such-boot-images"
        monkeypatch.setattr(sys, "argv", [
            "astra-boot", str(missing), "-M", "manifest.yaml", "-T", str(tmp_path / "work")])

        # The boot images path does not exist, so boot() stops before any USB work.
        assert boot.main() == -1
        err = capsys.readouterr().err
        assert "--manifest is ignored" in err

    def test_no_warning_without_manifest(self, tmp_path, monkeypatch, capsys):
        import astra_update.boot as boot

        monkeypatch.setattr(boot.signal, "signal", lambda *args: None)
        missing = tmp_path / "no-such-boot-images"
        monkeypatch.setattr(sys, "argv", [
            "astra-boot", str(missing), "-T", str(tmp_path / "work")])

        assert boot.main() == -1
        assert "--manifest" not in capsys.readouterr().err


class _FactorySpy:
    """Replaces FlashImage in astra_update.images and records factory calls."""

    def __init__(self):
        self.calls = []

    def flash_image_factory(self, path, config, manifest):
        self.calls.append((path, dict(config), manifest))
        image = Mock()
        image.load.return_value = 0
        return image


class TestTypedFlashImageLoaders:
    @pytest.mark.parametrize("loader_name,image_type", [
        ("load_spi_flash_image", "spi"),
        ("load_emmc_flash_image", "emmc"),
    ])
    def test_typed_loader_sets_image_type(self, loader_name, image_type, tmp_path, monkeypatch):
        import astra_update.images as images

        spy = _FactorySpy()
        monkeypatch.setattr(images, "FlashImage", spy)
        image_file = tmp_path / "image.bin"
        image_file.write_bytes(b"x")
        config = {"board": "any"}

        getattr(images, loader_name)(str(image_file), config, "manifest.yaml")

        assert spy.calls == [(str(image_file), {"board": "any", "image_type": image_type},
                              "manifest.yaml")]
        assert config == {"board": "any"}  # the caller's dict is not modified

    def test_typed_loader_accepts_a_matching_image_type(self, tmp_path, monkeypatch):
        import astra_update.images as images

        spy = _FactorySpy()
        monkeypatch.setattr(images, "FlashImage", spy)
        image_file = tmp_path / "image.bin"
        image_file.write_bytes(b"x")

        images.load_spi_flash_image(str(image_file), {"image_type": "SPI"})
        assert spy.calls[0][1]["image_type"] == "spi"

    def test_typed_loader_rejects_a_conflicting_image_type(self, tmp_path, monkeypatch):
        import astra_update.images as images

        spy = _FactorySpy()
        monkeypatch.setattr(images, "FlashImage", spy)
        image_file = tmp_path / "image.bin"
        image_file.write_bytes(b"x")

        with pytest.raises(ValueError, match="conflicts"):
            images.load_emmc_flash_image(str(image_file), {"image_type": "spi"})
        assert spy.calls == []


class TestManagerDestructorReleasesGil:
    def test_other_threads_run_while_a_manager_is_destroyed(self, tmp_path):
        """Smoke test: destroying a manager (which runs Shutdown()) works with
        another Python thread active, and returns."""
        import threading
        from astra_update import AstraDeviceManager

        stop = threading.Event()
        ticks = []

        def spin():
            while not stop.is_set():
                ticks.append(1)
                stop.wait(0.001)

        worker = threading.Thread(target=spin)
        worker.start()
        try:
            manager = AstraDeviceManager(temp_dir=str(tmp_path))
            del manager
        finally:
            stop.set()
            worker.join(timeout=5)
        assert not worker.is_alive()
