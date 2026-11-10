"""
pytest configuration and fixtures for astra-update Python tests
"""

import pytest
import sys
from pathlib import Path

# Add python to Python path for test imports
sys.path.insert(0, str(Path(__file__).parent.parent / "python"))


@pytest.fixture
def mock_device_manager():
    """Fixture providing a mock AstraDeviceManager."""
    from unittest.mock import MagicMock
    
    mgr = MagicMock()
    mgr.get_response.return_value = None
    mgr.get_responses.return_value = []
    mgr.has_responses.return_value = False
    mgr.response_queue_size.return_value = 0
    
    return mgr


@pytest.fixture
def temp_dir(tmp_path):
    """Fixture providing a temporary directory."""
    return tmp_path


def pytest_configure(config):
    """Configure pytest."""
    # Add custom markers
    config.addinivalue_line(
        "markers",
        "integration: mark test as requiring real devices (deselect with '-m \"not integration\"')"
    )
    config.addinivalue_line(
        "markers",
        "slow: mark test as slow running"
    )


def pytest_collection_modifyitems(config, items):
    """Modify test collection to skip unavailable modules."""
    # Check if C++ module is available
    try:
        import astra_update  # noqa:F401
        has_bindings = True
    except ImportError:
        has_bindings = False
    
    if not has_bindings:
        skip_marker = pytest.mark.skip(
            reason="C++ astra_update module not built"
        )
        for item in items:
            # Skip binding tests but keep unit tests
            if "test_bindings" in str(item.fspath):
                item.add_marker(skip_marker)
