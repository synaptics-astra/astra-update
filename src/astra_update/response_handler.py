"""
Response handler for AstraDeviceManager

Provides a Pythonic interface to the C++ response queue with event typing
and convenient status checking methods.
"""

from enum import Enum
from typing import Optional, Union, List
import threading
import time
from dataclasses import dataclass

# Import from package-local native extension module
from ._astra_update import (
    DeviceManagerResponse,
    DeviceResponse,
    ManagerResponse,
    DeviceStatus,
    DeviceManagerStatus,
)


class EventType(Enum):
    """Type of event received from the device manager."""
    MANAGER_EVENT = "manager"
    DEVICE_EVENT = "device"


@dataclass
class Event:
    """Represents a single event from the device manager."""
    event_type: EventType
    timestamp: float
    manager_response: Union[ManagerResponse, DeviceResponse, None] = None

    @classmethod
    def from_response(cls, response: DeviceManagerResponse) -> 'Event':
        """Create an Event from a DeviceManagerResponse."""
        event_type = EventType.MANAGER_EVENT if response.is_device_manager_response() else EventType.DEVICE_EVENT
        
        if response.is_device_manager_response():
            manager_resp = response.get_device_manager_response()
        else:
            manager_resp = response.get_device_response()
        
        return cls(
            event_type=event_type,
            timestamp=time.time(),
            manager_response=manager_resp
        )

    def is_manager_event(self) -> bool:
        """Check if this is a manager event."""
        return self.event_type == EventType.MANAGER_EVENT

    def is_device_event(self) -> bool:
        """Check if this is a device event."""
        return self.event_type == EventType.DEVICE_EVENT

    def get_manager_response(self) -> Optional[ManagerResponse]:
        """Get the manager response if this is a manager event."""
        if self.is_manager_event():
            return self.manager_response
        return None

    def get_device_response(self) -> Optional[DeviceResponse]:
        """Get the device response if this is a device event."""
        if self.is_device_event():
            return self.manager_response
        return None


class ResponseHandler:
    """
    High-level handler for device manager responses.
    
    Provides methods to poll and consume responses from the device manager,
    with optional callbacks for different event types.
    """

    def __init__(self, device_manager, poll_interval: float = 0.01):
        """
        Initialize the response handler.
        
        Args:
            device_manager: The AstraDeviceManager instance
            poll_interval: Time in seconds to wait between polls (default: 10ms)
        """
        self.device_manager = device_manager
        self.poll_interval = poll_interval
        self._callbacks = {}
        self._running = False
        self._thread = None

    def register_callback(self, event_type: EventType, callback):
        """
        Register a callback for a specific event type.
        
        Args:
            event_type: The EventType to listen for
            callback: Function to call when event occurs (receives Event object)
        """
        if event_type not in self._callbacks:
            self._callbacks[event_type] = []
        self._callbacks[event_type].append(callback)

    def unregister_callback(self, event_type: EventType, callback):
        """Unregister a previously registered callback."""
        if event_type in self._callbacks:
            self._callbacks[event_type].remove(callback)

    def _invoke_callbacks(self, event: Event):
        """Invoke registered callbacks for the event."""
        callbacks = self._callbacks.get(event.event_type, [])
        for callback in callbacks:
            try:
                callback(event)
            except Exception as e:
                print(f"Error in callback: {e}")

    def poll_responses(self) -> List[Event]:
        """
        Poll for all available responses without blocking.
        
        Returns:
            List of Event objects
        """
        events = []
        responses = self.device_manager.get_responses()
        for response in responses:
            event = Event.from_response(response)
            events.append(event)
            self._invoke_callbacks(event)
        return events

    def get_next_response(self, timeout: Optional[float] = None) -> Optional[Event]:
        """
        Get the next response, with optional timeout.
        
        Args:
            timeout: Time in seconds to wait for a response (None = indefinite)
            
        Returns:
            Event object or None if timeout
        """
        start_time = time.time()
        
        while True:
            response = self.device_manager.get_response()
            if response is not None:
                event = Event.from_response(response)
                self._invoke_callbacks(event)
                return event
            
            if timeout is not None:
                elapsed = time.time() - start_time
                if elapsed >= timeout:
                    return None
            
            time.sleep(self.poll_interval)

    def wait_for_status(
        self,
        device_name: str,
        target_status: DeviceStatus,
        timeout: Optional[float] = None
    ) -> bool:
        """
        Wait for a device to reach a specific status.
        
        Args:
            device_name: Name of the target device
            target_status: The status to wait for
            timeout: Maximum time to wait in seconds
            
        Returns:
            True if status reached, False if timeout
        """
        start_time = time.time()
        
        while True:
            event = self.get_next_response(timeout=0.1 if timeout is None else min(0.1, timeout))
            
            if event and event.is_device_event():
                dev_resp = event.get_device_response()
                if dev_resp and dev_resp.device_name == device_name and dev_resp.status == target_status:
                    return True
            
            if timeout is not None:
                elapsed = time.time() - start_time
                if elapsed >= timeout:
                    return False

    def wait_for_manager_shutdown(self, timeout: Optional[float] = None) -> bool:
        """
        Wait for the device manager to signal shutdown.
        
        Args:
            timeout: Maximum time to wait in seconds
            
        Returns:
            True if shutdown received, False if timeout
        """
        start_time = time.time()
        
        while True:
            event = self.get_next_response(timeout=0.1 if timeout is None else min(0.1, timeout))
            
            if event and event.is_manager_event():
                mgr_resp = event.get_manager_response()
                if mgr_resp and mgr_resp.manager_status == DeviceManagerStatus.SHUTDOWN:
                    return True
            
            if timeout is not None:
                elapsed = time.time() - start_time
                if elapsed >= timeout:
                    return False

    def has_responses(self) -> bool:
        """Check if there are any pending responses."""
        return self.device_manager.has_responses()

    def drainresponses(self) -> List[Event]:
        """
        Drain all pending responses and return them.
        
        Returns:
            List of all pending Event objects
        """
        all_events = []
        while self.has_responses():
            events = self.poll_responses()
            all_events.extend(events)
        return all_events
