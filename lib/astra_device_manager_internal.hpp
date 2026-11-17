// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "astra_device.hpp"
#include "image.hpp"
#include "usb_transport.hpp"

// Creates the transport AstraDeviceManager uses for a given transport type.
using USBTransportFactory =
    std::function<std::shared_ptr<USBTransport>(AstraTransportType transportType, bool usbDebug)>;

// Test hook: when set, AstraDeviceManager obtains its transports from this
// factory instead of the platform libusb/CDC transports. Set it before
// Boot()/Update(); pass nullptr to restore the default.
void SetUSBTransportFactoryForTesting(USBTransportFactory factory);

// The command appended to an update's flash command, or "" for none.
// With reset enabled: reset (or, for SL26xx boot images that support it,
// re-enter fastboot so the host can confirm the write and reboot). With reset
// disabled, SL26xx images that support it still re-enter fastboot, because
// that is the only signal that the eMMC write finished; the host skips the
// reboot. Everything else appends nothing.
std::string FlashCompletionCommand(AstraDeviceSeries series, bool fastbootReboot, bool resetWhenComplete);

// An SL26xx update device thread waiting, after Boot(), for its board to
// re-enumerate as a fastboot device.
struct FastbootArrival {
    std::chrono::steady_clock::time_point deadline;
    // USB path of the device that is re-enumerating.
    std::string usbPath;
    bool arrived = false;
};

// Picks the pending arrival that a fastboot device on |usbPath| satisfies,
// marks it arrived and removes it from |pending|. A board normally
// re-enumerates on the same port, so the entry with the same USB path is
// preferred; if none has it (a CDC primary transport reports a tty path, or
// the path changed), the oldest entry is taken. Returns nullptr if |pending|
// is empty. The caller holds the lock guarding |pending|.
std::shared_ptr<FastbootArrival> ClaimFastbootArrival(
    std::deque<std::shared_ptr<FastbootArrival>> &pending, const std::string &usbPath);
