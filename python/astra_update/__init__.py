"""
Astra Update Python Bindings

This package provides Python access to the astra-update library through pybind11.
"""

# Import core bindings from package-local native C++ extension module.
from ._astra_update import (
    AstraDeviceManager,
    AstraBootImage,
    DeviceManagerResponse,
    DeviceResponse,
    ManagerResponse,
    
    # Enums
    LogLevel,
    DeviceBootStage,
    DeviceStatus,
    DeviceSeries,
    DeviceManagerStatus,
    
    # Utilities
    AstraDevice,
    FlashImage,
    Image,
    FlashImageType,
    ImageType,
    SecureBootVersion,
    MemoryLayout,
    MemoryDDRType,
    TransportType,
    UbootConsole,
    UbootVariant,
    UbootVersion,
    ResponseQueue,
)

__version__ = AstraDeviceManager.get_version()

# Import Python-level utilities
from .response_handler import ResponseHandler, EventType, Event

__all__ = [
    # Classes
    'AstraDeviceManager',
    'AstraBootImage',
    'DeviceManagerResponse',
    'DeviceResponse',
    'ManagerResponse',
    'AstraDevice',
    'FlashImage',
    'Image',
    'ResponseQueue',
    
    # Enums
    'LogLevel',
    'DeviceBootStage',
    'DeviceStatus',
    'DeviceSeries',
    'DeviceManagerStatus',
    'FlashImageType',
    'ImageType',
    'SecureBootVersion',
    'MemoryLayout',
    'MemoryDDRType',
    'TransportType',
    'UbootConsole',
    'UbootVariant',
    'UbootVersion',
    
    # Response utilities
    'ResponseHandler',
    'EventType',
    'Event',
]
