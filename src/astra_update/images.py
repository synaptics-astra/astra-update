"""
Image loaders for FlashImage and AstraBootImage

Provides convenient Python interfaces to create and load boot/flash images
from the C++ factories.
"""

from typing import Optional
from pathlib import Path

from astra_update import FlashImage


def load_flash_image(
    image_path: str,
    config_path: Optional[str] = None,
    manifest_path: Optional[str] = None
) -> FlashImage:
    """
    Load a flash image from file.
    
    This is a wrapper around the C++ FlashImage::FlashImageFactory method.
    
    Args:
        image_path: Path to the flash image file (required)
        config_path: Path to the configuration file (optional)
        manifest_path: Path to the manifest file (optional)
        
    Returns:
        FlashImage object
        
    Raises:
        FileNotFoundError: If image_path doesn't exist
        RuntimeError: If image loading fails
    """
    # Verify the image file exists
    img_path = Path(image_path)
    if not img_path.exists():
        raise FileNotFoundError(f"Image file not found: {image_path}")
    
    # Convert paths to strings if they're Path objects
    img_str = str(img_path)
    cfg_str = str(config_path) if config_path else ""
    mani_str = str(manifest_path) if manifest_path else ""
    
    try:
        image = FlashImage.flash_image_factory(img_str, cfg_str, mani_str)
        if image is None:
            raise RuntimeError(f"Failed to load flash image from {image_path}")
        return image
    except Exception as e:
        raise RuntimeError(f"Error loading flash image: {e}")


def load_spi_flash_image(
    image_path: str,
    config_path: Optional[str] = None,
    manifest_path: Optional[str] = None
) -> FlashImage:
    """
    Load a SPI flash image from file.
    
    Convenience wrapper for loading SPI flash images.
    
    Args:
        image_path: Path to the SPI flash image file
        config_path: Path to the configuration file (optional)
        manifest_path: Path to the manifest file (optional)
        
    Returns:
        FlashImage object for SPI flash
    """
    return load_flash_image(image_path, config_path, manifest_path)


def load_emmc_flash_image(
    image_path: str,
    config_path: Optional[str] = None,
    manifest_path: Optional[str] = None
) -> FlashImage:
    """
    Load an eMMC flash image from file.
    
    Convenience wrapper for loading eMMC flash images.
    
    Args:
        image_path: Path to the eMMC flash image file
        config_path: Path to the configuration file (optional)
        manifest_path: Path to the manifest file (optional)
        
    Returns:
        FlashImage object for eMMC flash
    """
    return load_flash_image(image_path, config_path, manifest_path)


__all__ = [
    'load_flash_image',
    'load_spi_flash_image',
    'load_emmc_flash_image',
]
