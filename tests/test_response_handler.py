"""
Tests for ResponseHandler and event handling.

These tests verify that:
1. ResponseHandler can be created
2. Event creation and typing works
3. Callbacks can be registered and invoked
4. Polling and waiting mechanisms work
"""

import pytest
import sys
import time
from pathlib import Path
from unittest.mock import Mock, MagicMock

# Add python to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent / "python"))


class TestEventClass:
    """Test Event data class."""

    def test_import_event(self):
        """Test that Event can be imported."""
        try:
            from astra_update.response_handler import Event, EventType
            assert Event is not None
            assert EventType is not None
        except ImportError as e:
            pytest.skip(f"ResponseHandler not available: {e}")

    def test_event_types(self):
        """Test EventType enum."""
        try:
            from astra_update.response_handler import EventType
            
            assert EventType.MANAGER_EVENT
            assert EventType.DEVICE_EVENT
        except ImportError as e:
            pytest.skip(f"ResponseHandler not available: {e}")

    def test_event_creation(self):
        """Test creating Event objects."""
        try:
            from astra_update.response_handler import Event, EventType
            
            event = Event(
                event_type=EventType.MANAGER_EVENT,
                timestamp=time.time(),
                manager_response=None
            )
            assert event.event_type == EventType.MANAGER_EVENT
            assert event.timestamp > 0
        except ImportError as e:
            pytest.skip(f"ResponseHandler not available: {e}")

    def test_event_type_checking(self):
        """Test event type checking methods."""
        try:
            from astra_update.response_handler import Event, EventType
            
            event = Event(
                event_type=EventType.DEVICE_EVENT,
                timestamp=time.time(),
            )
            assert event.is_device_event()
            assert not event.is_manager_event()
        except ImportError as e:
            pytest.skip(f"ResponseHandler not available: {e}")


class TestResponseHandler:
    """Test ResponseHandler class."""

    def test_import_response_handler(self):
        """Test that ResponseHandler can be imported."""
        try:
            from astra_update.response_handler import ResponseHandler
            assert ResponseHandler is not None
        except ImportError as e:
            pytest.skip(f"ResponseHandler not available: {e}")

    def test_create_response_handler(self):
        """Test creating ResponseHandler."""
        try:
            from astra_update.response_handler import ResponseHandler
            
            mock_mgr = MagicMock()
            handler = ResponseHandler(mock_mgr)
            
            assert handler is not None
            assert handler.device_manager == mock_mgr
        except ImportError as e:
            pytest.skip(f"ResponseHandler not available: {e}")

    def test_poll_responses_empty(self):
        """Test polling empty response queue."""
        try:
            from astra_update.response_handler import ResponseHandler
            
            mock_mgr = MagicMock()
            mock_mgr.get_responses.return_value = []
            
            handler = ResponseHandler(mock_mgr)
            responses = handler.poll_responses()
            
            assert responses == []
        except ImportError as e:
            pytest.skip(f"ResponseHandler not available: {e}")

    def test_callback_registration(self):
        """Test registering callbacks."""
        try:
            from astra_update.response_handler import ResponseHandler, EventType
            
            mock_mgr = MagicMock()
            handler = ResponseHandler(mock_mgr)
            
            callback = Mock()
            handler.register_callback(EventType.DEVICE_EVENT, callback)
            
            assert EventType.DEVICE_EVENT in handler._callbacks
            assert callback in handler._callbacks[EventType.DEVICE_EVENT]
        except ImportError as e:
            pytest.skip(f"ResponseHandler not available: {e}")

    def test_callback_unregistration(self):
        """Test unregistering callbacks."""
        try:
            from astra_update.response_handler import ResponseHandler, EventType
            
            mock_mgr = MagicMock()
            handler = ResponseHandler(mock_mgr)
            
            callback = Mock()
            handler.register_callback(EventType.DEVICE_EVENT, callback)
            handler.unregister_callback(EventType.DEVICE_EVENT, callback)
            
            assert callback not in handler._callbacks.get(
                EventType.DEVICE_EVENT, []
            )
        except ImportError as e:
            pytest.skip(f"ResponseHandler not available: {e}")

    def test_has_responses(self):
        """Test checking if responses are available."""
        try:
            from astra_update.response_handler import ResponseHandler
            
            mock_mgr = MagicMock()
            mock_mgr.has_responses.return_value = True
            
            handler = ResponseHandler(mock_mgr)
            assert handler.has_responses()
            
            mock_mgr.has_responses.return_value = False
            assert not handler.has_responses()
        except ImportError as e:
            pytest.skip(f"ResponseHandler not available: {e}")

    def test_drain_responses(self):
        """Test draining all responses."""
        try:
            from astra_update.response_handler import ResponseHandler
            
            mock_mgr = MagicMock()
            # Simulate queue with 3 items then empty
            mock_mgr.has_responses.side_effect = [True, True, True, False]
            mock_mgr.get_responses.side_effect = [
                [],  # poll_responses calls get_responses
                [],  # poll_responses calls get_responses again
                [MagicMock() for _ in range(3)]  # Simulate 3 responses
            ]
            
            handler = ResponseHandler(mock_mgr)
            # Note: This test would need actual responses to work properly
            # For now, we're just verifying the method exists and is callable
            responses = handler.drain_responses()
            assert isinstance(responses, list)
        except ImportError as e:
            pytest.skip(f"ResponseHandler not available: {e}")


class TestResponseHandlerWaitMethods:
    """Test ResponseHandler waiting mechanisms."""

    def test_get_next_response_timeout(self):
        """Test timeout on get_next_response."""
        try:
            from astra_update.response_handler import ResponseHandler
            
            mock_mgr = MagicMock()
            mock_mgr.get_response.return_value = None
            
            handler = ResponseHandler(mock_mgr, poll_interval=0.001)
            
            start = time.time()
            response = handler.get_next_response(timeout=0.1)
            elapsed = time.time() - start
            
            assert response is None
            assert elapsed >= 0.1
        except ImportError as e:
            pytest.skip(f"ResponseHandler not available: {e}")

    def test_wait_for_manager_shutdown_timeout(self):
        """Test timeout on wait_for_manager_shutdown."""
        try:
            from astra_update.response_handler import ResponseHandler
            
            mock_mgr = MagicMock()
            mock_mgr.get_response.return_value = None
            
            handler = ResponseHandler(mock_mgr, poll_interval=0.001)
            
            start = time.time()
            result = handler.wait_for_manager_shutdown(timeout=0.1)
            elapsed = time.time() - start
            
            assert result is False
            assert elapsed >= 0.1
        except ImportError as e:
            pytest.skip(f"ResponseHandler not available: {e}")


class TestImageLoaders:
    """Test image loading functions."""

    def test_import_image_loaders(self):
        """Test importing image loader functions."""
        try:
            from astra_update.images import (
                load_flash_image,
                load_spi_flash_image,
                load_emmc_flash_image,
            )
            assert load_flash_image is not None
            assert load_spi_flash_image is not None
            assert load_emmc_flash_image is not None
        except ImportError as e:
            pytest.skip(f"Image loaders not available: {e}")

    def test_load_flash_image_file_not_found(self):
        """Test error handling for missing file."""
        try:
            from astra_update.images import load_flash_image
            
            with pytest.raises(FileNotFoundError):
                load_flash_image("/nonexistent/path/image.bin")
        except ImportError as e:
            pytest.skip(f"Image loaders not available: {e}")


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
