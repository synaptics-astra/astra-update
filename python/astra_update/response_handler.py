"""
Response handler for AstraDeviceManager

Provides a Pythonic interface to the C++ response queue with event typing
and convenient status checking methods.
"""

from collections import deque
from enum import Enum
from typing import Callable, Optional, Union, List
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


# Device statuses after which the device will not report anything further for
# the current operation. These match the statuses the C++ manager counts as
# failures. IMAGE_SEND_FAIL is not included: after a boot, a device can report
# it for one image and keep serving the rest.
_DEVICE_FAILURE_STATUSES = frozenset((
    DeviceStatus.OPEN_FAIL,
    DeviceStatus.BOOT_FAIL,
    DeviceStatus.UPDATE_FAIL,
))

# Manager statuses after which no further device status can arrive.
_MANAGER_STOP_STATUSES = frozenset((
    DeviceManagerStatus.FAILURE,
    DeviceManagerStatus.SHUTDOWN,
))


class WaitAbortedError(RuntimeError):
    """
    Raised by ResponseHandler.wait_for_status() when the awaited status can no
    longer arrive: the device reported a failure, or the manager reported a
    failure or shut down.

    The event that ended the wait is available as ``event``.
    """

    def __init__(self, event: 'Event'):
        self.event = event
        if event.is_device_event():
            response = event.get_device_response()
            message = (f"Device {response.device_name} reported {response.status.name}"
                       f": {response.message}")
        else:
            response = event.get_manager_response()
            message = (f"Device manager reported {response.manager_status.name}"
                       f": {response.manager_message}")
        super().__init__(message)


class ResponseHandler:
    """
    High-level handler for device manager responses.
    
    Provides methods to poll and consume responses from the device manager,
    with optional callbacks for different event types.

    Callbacks run once for each response, when it is taken from the device
    manager. The wait methods set aside the events they pass over, and
    get_next_response(), poll_responses() and drain_responses() return those
    events first, in the order they arrived, so waiting never loses an event.
    A ResponseHandler is not thread safe; use it from one thread.
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
        # Events taken from the device manager by a wait method but not
        # matched by it, oldest first.
        self._pending = deque()

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

    def _receive(self) -> Optional[Event]:
        """Take one response from the device manager and run its callbacks."""
        response = self.device_manager.get_response()
        if response is None:
            return None
        event = Event.from_response(response)
        self._invoke_callbacks(event)
        return event

    def poll_responses(self) -> List[Event]:
        """
        Poll for all available responses without blocking.
        
        Returns:
            List of Event objects, starting with any set aside by a wait method
        """
        events = list(self._pending)
        self._pending.clear()
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
        if self._pending:
            return self._pending.popleft()

        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            event = self._receive()
            if event is not None:
                return event
            if deadline is not None and time.monotonic() >= deadline:
                return None
            time.sleep(self.poll_interval)

    def _wait_for(
        self,
        matches: Callable[[Event], bool],
        aborts: Callable[[Event], bool],
        timeout: Optional[float],
    ) -> bool:
        """
        Consume events in arrival order until one matches or aborts the wait.

        The matching or aborting event is consumed; every other event is kept
        for the caller. Returns True on a match and False on timeout, and
        raises WaitAbortedError on an aborting event.
        """
        deadline = None if timeout is None else time.monotonic() + timeout

        # Events set aside by an earlier wait arrived first, so check them first.
        for index, event in enumerate(self._pending):
            if matches(event):
                del self._pending[index]
                return True
            if aborts(event):
                del self._pending[index]
                raise WaitAbortedError(event)

        while True:
            event = self._receive()
            if event is not None:
                if matches(event):
                    return True
                if aborts(event):
                    raise WaitAbortedError(event)
                self._pending.append(event)
                continue
            if deadline is not None and time.monotonic() >= deadline:
                return False
            time.sleep(self.poll_interval)

    def wait_for_status(
        self,
        device_name: str,
        target_status: DeviceStatus,
        timeout: Optional[float] = None
    ) -> bool:
        """
        Wait for a device to reach a specific status.

        Events for other devices, and other events for this device, are kept
        and returned later by get_next_response(), poll_responses() and
        drain_responses().
        
        Args:
            device_name: Name of the target device
            target_status: The status to wait for
            timeout: Maximum time to wait in seconds (None = indefinite)
            
        Returns:
            True if status reached, False if timeout

        Raises:
            WaitAbortedError: if, before target_status, the device reports
                OPEN_FAIL, BOOT_FAIL or UPDATE_FAIL, or the manager reports
                FAILURE or SHUTDOWN. The error's ``event`` is that event.
        """
        def device_response(event: Event) -> Optional[DeviceResponse]:
            response = event.get_device_response()
            if response is not None and response.device_name == device_name:
                return response
            return None

        def matches(event: Event) -> bool:
            response = device_response(event)
            return response is not None and response.status == target_status

        def aborts(event: Event) -> bool:
            if event.is_manager_event():
                return event.get_manager_response().manager_status in _MANAGER_STOP_STATUSES
            response = device_response(event)
            return response is not None and response.status in _DEVICE_FAILURE_STATUSES

        return self._wait_for(matches, aborts, timeout)

    def wait_for_manager_shutdown(self, timeout: Optional[float] = None) -> bool:
        """
        Wait for the device manager to signal shutdown.

        Device events and other manager events are kept and returned later by
        get_next_response(), poll_responses() and drain_responses().
        
        Args:
            timeout: Maximum time to wait in seconds (None = indefinite)
            
        Returns:
            True if shutdown received, False if timeout
        """
        def matches(event: Event) -> bool:
            return (event.is_manager_event() and
                    event.get_manager_response().manager_status == DeviceManagerStatus.SHUTDOWN)

        return self._wait_for(matches, lambda event: False, timeout)

    def has_responses(self) -> bool:
        """Check if there are any pending responses, including set-aside ones."""
        return bool(self._pending) or self.device_manager.has_responses()

    def drain_responses(self) -> List[Event]:
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

    drainresponses = drain_responses
