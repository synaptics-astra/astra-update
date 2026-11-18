"""
Image loaders for FlashImage and AstraBootImage

Provides convenient Python interfaces to create and load boot/flash images
from the C++ factories.
"""

from typing import Optional
from pathlib import Path

from ._astra_update import FlashImage


def load_flash_image(
    image_path: str,
    config: Optional[dict] = None,
    manifest: Optional[str] = None
) -> FlashImage:
    """
    Load a flash image from file.

    This is a wrapper around the C++ FlashImage::FlashImageFactory method.

    Args:
        image_path: Path to the flash image file (required)
        config: Optional dict of config overrides passed to the factory.
                Recognised keys: board, chip, image_type, boot_image,
                secure_boot, memory_layout, ddr_type, reset
        manifest: Path to the manifest file (optional)

    Returns:
        FlashImage object

    Raises:
        FileNotFoundError: If image_path doesn't exist
        RuntimeError: If image loading fails
    """
    img_path = Path(image_path)
    if not img_path.exists():
        raise FileNotFoundError(f"Image file not found: {image_path}")

    cfg = config or {}
    mani_str = str(manifest) if manifest else ""

    try:
        image = FlashImage.flash_image_factory(str(img_path), cfg, mani_str)
        if image is None:
            raise RuntimeError(f"Failed to load flash image from {image_path}")
        if image.load() != 0:
            raise RuntimeError(image.get_load_error() or f"Failed to load flash image from {image_path}")
        return image
    except Exception as e:
        raise RuntimeError(f"Error loading flash image: {e}") from e


def _load_typed_flash_image(
    image_type: str,
    image_path: str,
    config: Optional[dict],
    manifest: Optional[str],
) -> FlashImage:
    """Load a flash image with config["image_type"] set to image_type."""
    cfg = dict(config or {})
    requested = cfg.get("image_type")
    if requested is not None and str(requested).lower() != image_type:
        raise ValueError(
            f"config image_type {requested!r} conflicts with load_{image_type}_flash_image()")
    cfg["image_type"] = image_type
    return load_flash_image(image_path, cfg, manifest)


def load_spi_flash_image(
    image_path: str,
    config: Optional[dict] = None,
    manifest: Optional[str] = None
) -> FlashImage:
    """
    Load a SPI flash image from file.

    Same as load_flash_image() with config["image_type"] set to "spi", so the
    image is loaded as SPI whatever its manifest or file layout suggests.

    Args:
        image_path: Path to the SPI flash image file
        config: Optional dict of config overrides (see load_flash_image)
        manifest: Path to the manifest file (optional)

    Returns:
        FlashImage object for SPI flash

    Raises:
        ValueError: If config sets a different image_type
        FileNotFoundError, RuntimeError: As for load_flash_image
    """
    return _load_typed_flash_image("spi", image_path, config, manifest)


def load_emmc_flash_image(
    image_path: str,
    config: Optional[dict] = None,
    manifest: Optional[str] = None
) -> FlashImage:
    """
    Load an eMMC flash image from file.

    Same as load_flash_image() with config["image_type"] set to "emmc", so the
    image is loaded as eMMC whatever its manifest or file layout suggests.

    Args:
        image_path: Path to the eMMC flash image directory
        config: Optional dict of config overrides (see load_flash_image)
        manifest: Path to the manifest file (optional)

    Returns:
        FlashImage object for eMMC flash

    Raises:
        ValueError: If config sets a different image_type
        FileNotFoundError, RuntimeError: As for load_flash_image
    """
    return _load_typed_flash_image("emmc", image_path, config, manifest)


__all__ = [
    'load_flash_image',
    'load_spi_flash_image',
    'load_emmc_flash_image',
]
