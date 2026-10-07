#!/usr/bin/env python3
"""
Astra Boot Tool - Python Implementation

Mirrors the functionality of the C++ astra-boot.cpp utility using the
astra-update Python bindings.

Usage:
    python python/astra-boot.py <boot-image> [options]
"""

import argparse
import sys
import signal
from pathlib import Path
from typing import Dict, Optional
from dataclasses import dataclass
import time

try:
    from tqdm import tqdm
except ImportError:
    tqdm = None

from astra_update import (
    __version__,
    AstraDeviceManager,
    DeviceBootStage,
    DeviceStatus,
    DeviceManagerStatus,
    LogLevel,
    AstraDevice,
)
from astra_update.response_handler import ResponseHandler, EventType

# Progress tracking
@dataclass
class ProgressBar:
    """Tracks progress for a device/image pair."""
    device_name: str
    image_name: str
    tqdm_obj: Optional[tqdm] = None
    last_progress: int = 0


class AstraBootTool:
    """Python implementation of astra-boot utility."""

    def __init__(
        self,
        boot_images_path: str,
        boot_command: str = "",
        boot_stage: DeviceBootStage = DeviceBootStage.AUTO,
        log_file: str = "",
        debug: bool = False,
        continuous: bool = False,
        exit_on_error: bool = False,
        usb_debug: bool = False,
        simple_progress: bool = False,
        temp_dir: str = "",
        filter_ports: str = "",
        keep_image_request_loop_after_boot: bool = False,
        leave_fastboot_idle: bool = False,
    ):
        """Initialize the boot tool."""
        self.boot_images_path = boot_images_path
        self.boot_command = boot_command
        self.boot_stage = boot_stage
        self.log_file = log_file
        self.debug = debug
        self.continuous = continuous
        self.exit_on_error = exit_on_error
        self.usb_debug = usb_debug
        self.simple_progress = simple_progress or usb_debug  # USB debug forces simple progress
        self.temp_dir = temp_dir
        self.filter_ports = filter_ports
        
        # Setup logging level
        log_level = LogLevel.DEBUG if debug else LogLevel.INFO
        
        # Create device manager with automatic response queue callback
        self.device_manager = AstraDeviceManager(
            continuous=continuous,
            log_level=log_level,
            log_path=log_file,
            temp_dir=temp_dir,
            filter_ports=filter_ports,
            usb_debug=self.usb_debug,
            keep_image_request_loop_after_boot=keep_image_request_loop_after_boot,
            leave_fastboot_idle=leave_fastboot_idle,
        )
        
        # Create response handler
        self.response_handler = ResponseHandler(
            self.device_manager,
            poll_interval=0.01
        )
        
        # Progress tracking
        self.progress_bars: Dict[str, ProgressBar] = {}
        
        # Control flag
        self.running = True
        
    def setup_signal_handlers(self):
        """Setup signal handlers for graceful shutdown."""
        def signal_handler(signum, frame):
            print("\n[INFO] Received interrupt signal, shutting down...", file=sys.stderr)
            self.running = False
        
        signal.signal(signal.SIGINT, signal_handler)

    def boot(self) -> int:
        """
        Boot Astra devices with the specified boot images.
        
        Returns:
            0 on success, non-zero on error
        """
        self.setup_signal_handlers()
        
        print("Astra Boot\n")
        
        # Verify boot image path exists
        if not Path(self.boot_images_path).exists():
            print(f"[ERROR] Boot images path does not exist: {self.boot_images_path}", 
                  file=sys.stderr)
            return -1
        
        try:
            # Start the boot operation
            self.device_manager.boot(
                self.boot_images_path,
                self.boot_command,
                self.boot_stage
            )
        except Exception as e:
            print(f"[ERROR] Failed to initialize boot: {e}", file=sys.stderr)
            return -1
        
        # Process responses
        try:
            while self.running:
                event = self.response_handler.get_next_response(timeout=0.1)
                
                if event is None:
                    continue
                
                if event.is_manager_event():
                    self._handle_manager_event(event)
                    if self._should_shutdown():
                        break
                elif event.is_device_event():
                    self._handle_device_event(event)
        finally:
            # Cleanup
            self._cleanup_progress_bars()
            
        # Shutdown device manager
        has_error = self.device_manager.shutdown()
        
        if has_error:
            log_file = self.device_manager.get_log_file()
            print(f"[ERROR] Device manager reported errors. Check log: {log_file}",
                  file=sys.stderr)
            return -1
        
        return 0

    def _handle_manager_event(self, event):
        """Handle a manager event."""
        response = event.get_manager_response()
        if not response:
            return
        
        status = response.manager_status
        message = response.manager_message
        
        if status == DeviceManagerStatus.INFO:
            print(f"[INFO] {message}")
        elif status == DeviceManagerStatus.START:
            print(f"[START] {message}")
        elif status == DeviceManagerStatus.FAILURE:
            print(f"[ERROR] {message}", file=sys.stderr)
        elif status == DeviceManagerStatus.SHUTDOWN:
            print("[INFO] Device manager shutting down")
            self.running = False

    def _handle_device_event(self, event):
        """Handle a device event."""
        response = event.get_device_response()
        if not response:
            return
        
        device_name = response.device_name
        status = response.status
        progress = response.progress
        image_name = response.image_name
        message = response.message
        
        if status == DeviceStatus.ADDED:
            print(f"[DEVICE] Detected: {device_name}")
        elif status == DeviceStatus.BOOT_START:
            print(f"[DEVICE] Booting: {device_name}")
        elif status == DeviceStatus.BOOT_COMPLETE:
            print(f"[DEVICE] Boot complete: {device_name}")
        elif status == DeviceStatus.BOOT_FAIL:
            print(f"[ERROR] Boot failed for {device_name}: {message}",
                  file=sys.stderr)
            if self.continuous and self.exit_on_error:
                self.running = False
        elif status in (DeviceStatus.IMAGE_SEND_START,
                       DeviceStatus.IMAGE_SEND_PROGRESS,
                       DeviceStatus.IMAGE_SEND_COMPLETE):
            self._update_progress(device_name, image_name, progress, status)

    def _update_progress(self, device_name: str, image_name: str, 
                        progress: float, status: DeviceStatus):
        """Update progress display for a device/image pair."""
        key = f"{device_name}:{image_name}"
        
        if self.simple_progress:
            print(f"[PROGRESS] Device: {device_name} Image: {image_name} "
                  f"Progress: {progress:.0f}%")
            return
        
        # Use tqdm if available
        if tqdm is None:
            print(f"[PROGRESS] Device: {device_name} Image: {image_name} "
                  f"Progress: {progress:.0f}%")
            return
        
        # Create or update progress bar
        if key not in self.progress_bars:
            pbar = tqdm(
                total=100,
                desc=f"{device_name}: {image_name}",
                unit="%",
                leave=False,
            )
            self.progress_bars[key] = ProgressBar(
                device_name=device_name,
                image_name=image_name,
                tqdm_obj=pbar,
            )
        
        pbar_obj = self.progress_bars[key]
        
        # Update progress
        delta = int(progress) - pbar_obj.last_progress
        if delta > 0:
            pbar_obj.tqdm_obj.update(delta)
            pbar_obj.last_progress = int(progress)
        
        # Mark as complete if done
        if status == DeviceStatus.IMAGE_SEND_COMPLETE:
            pbar_obj.tqdm_obj.close()
            del self.progress_bars[key]

    def _should_shutdown(self) -> bool:
        """Check if we should shutdown."""
        return not self.running

    def _cleanup_progress_bars(self):
        """Close all open progress bars."""
        for pbar_data in self.progress_bars.values():
            if pbar_data.tqdm_obj:
                pbar_data.tqdm_obj.close()
        self.progress_bars.clear()


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        prog="astra-boot",
        description="Astra USB Boot Utility"
    )
    
    parser.add_argument(
        "boot_image",
        nargs="?",
        help="Path to boot images"
    )
    
    parser.add_argument(
        "-l", "--log",
        default="",
        help="Log file path"
    )
    
    parser.add_argument(
        "-D", "--debug",
        action="store_true",
        help="Enable debug logging"
    )
    
    parser.add_argument(
        "-C", "--continuous",
        action="store_true",
        help="Enable updating multiple devices"
    )
    
    parser.add_argument(
        "-T", "--temp-dir",
        default="",
        help="Temporary directory"
    )
    
    parser.add_argument(
        "-M", "--manifest",
        default="",
        help="Manifest file path"
    )
    
    parser.add_argument(
        "-u", "--usb-debug",
        action="store_true",
        help="Enable USB debug logging"
    )
    
    parser.add_argument(
        "-S", "--simple-progress",
        action="store_true",
        help="Disable progress bars and report progress messages"
    )
    
    parser.add_argument(
        "-o", "--boot-command",
        default="",
        help="Boot command"
    )
    
    parser.add_argument(
        "-p", "--port",
        default="",
        help="Filter based on USB port"
    )
    
    parser.add_argument(
        "-e", "--exit-on-error",
        action="store_true",
        help="Exit if an error occurs when running in continuous mode"
    )
    
    parser.add_argument(
        "-b", "--boot-stage",
        default="auto",
        choices=["auto", "bootloader", "linux", "m52bl", "sysmgr"],
        help="Target boot stage"
    )
    
    parser.add_argument(
        "-k", "--keep-image-request-loop",
        action="store_true",
        help="Keep serving image requests after boot completes"
    )

    parser.add_argument(
        "--leave-fastboot-idle",
        action="store_true",
        help="Leave an SL26XX device idle in fastboot without serving image requests"
    )

    parser.add_argument(
        "-v", "--version",
        action="store_true",
        help="Print version"
    )
    
    args = parser.parse_args()
    
    # Handle version
    if args.version:
        lib_version = AstraDeviceManager.get_version()
        print(f"astra-boot: v{__version__} (lib v{lib_version})")
        return 0

    if args.boot_image is None:
        parser.error("the following arguments are required: boot_image")
    
    # Parse boot stage
    stage_map = {
        "auto": DeviceBootStage.AUTO,
        "bootloader": DeviceBootStage.BOOTLOADER,
        "linux": DeviceBootStage.LINUX,
        "m52bl": DeviceBootStage.M52BL,
        "sysmgr": DeviceBootStage.SYSMGR,
    }
    boot_stage = stage_map[args.boot_stage]
    
    # Create and run boot tool
    boot_tool = AstraBootTool(
        boot_images_path=args.boot_image,
        boot_command=args.boot_command,
        boot_stage=boot_stage,
        log_file=args.log,
        debug=args.debug,
        continuous=args.continuous,
        exit_on_error=args.exit_on_error,
        usb_debug=args.usb_debug,
        simple_progress=args.simple_progress,
        temp_dir=args.temp_dir,
        filter_ports=args.port,
        keep_image_request_loop_after_boot=args.keep_image_request_loop,
        leave_fastboot_idle=args.leave_fastboot_idle,
    )
    
    return boot_tool.boot()


if __name__ == "__main__":
    sys.exit(main())
