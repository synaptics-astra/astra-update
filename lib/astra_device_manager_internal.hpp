// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#pragma once

#include <functional>
#include <memory>

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
