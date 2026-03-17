"""
Astra Update Python Bindings

This package provides Python access to the astra-update library through pybind11.
"""

__version__ = "2.0.0-dev"

# Import core bindings from package-local native C++ extension module.
from ._astra_update import (
    AstraDeviceManager,
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
    ResponseQueue,
)

# Import Python-level utilities
from .response_handler import ResponseHandler, EventType, Event

__all__ = [
    # Classes
    'AstraDeviceManager',
    'DeviceManagerResponse',
    'DeviceResponse',
    'ManagerResponse',
    'AstraDevice',
    'FlashImage',
    'ResponseQueue',
    
    # Enums
    'LogLevel',
    'DeviceBootStage',
    'DeviceStatus',
    'DeviceSeries',
    'DeviceManagerStatus',
    
    # Response utilities
    'ResponseHandler',
    'EventType',
    'Event',
]
