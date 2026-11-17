"""
Tests for the Python astra-boot CLI (astra_update.boot) and the typed
flash-image loaders.
"""

import sys
from pathlib import Path
from unittest.mock import Mock

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "python"))


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
