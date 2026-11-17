// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#pragma once

#include <functional>
#include <memory>

#include "image.hpp"
#include "usb_transport.hpp"

// Creates the transport AstraDeviceManager uses for a given transport type.
using USBTransportFactory =
    std::function<std::shared_ptr<USBTransport>(AstraTransportType transportType, bool usbDebug)>;

// Test hook: when set, AstraDeviceManager obtains its transports from this
// factory instead of the platform libusb/CDC transports. Set it before
// Boot()/Update(); pass nullptr to restore the default.
void SetUSBTransportFactoryForTesting(USBTransportFactory factory);
