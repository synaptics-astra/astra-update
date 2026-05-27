// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

// Internal header shared between posix_usb_cdc_transport.cpp (shared methods)
// and the platform-specific implementation files.  Not part of the public
// library API.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct DeviceIdentityInfo {
    std::optional<uint16_t> vendorId;
    std::optional<uint16_t> productId;
    std::optional<uint32_t> numInterfaces;
    std::optional<uint32_t> interfaceNumber;
};

// Defined in exactly one of:
//   posix_usb_cdc_transport_linux.cpp  (PLATFORM_LINUX builds)
//   posix_usb_cdc_transport_macos.cpp  (PLATFORM_MACOS builds)
//
// Called from the shared posix_usb_cdc_transport.cpp (ProcessPendingDevices,
// MatchesVendorProduct).
std::optional<DeviceIdentityInfo> ReadPosixIdentity(const std::string &portPath);
