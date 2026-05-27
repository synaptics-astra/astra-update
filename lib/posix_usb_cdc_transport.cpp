// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated
//
// Shared (platform-independent) methods for PosixUSBCDCTransport.
// Platform-specific code lives in posix_usb_cdc_transport_linux.cpp and
// posix_usb_cdc_transport_macos.cpp; only one of those is compiled per build.

#include "posix_usb_cdc_transport.hpp"
#include "posix_usb_cdc_transport_impl.hpp"

#include <filesystem>

#include "astra_log.hpp"
#include "posix_usb_cdc_device.hpp"

// ProcessPendingDevices is used as a fallback on Linux (if the udev pipe setup
// fails) via the polling-based DeviceMonitorThread.  On macOS the IOKit
// notification path handles device detection; this function is kept as a
// no-op safety net but is not invoked under normal operation.
void PosixUSBCDCTransport::ProcessPendingDevices()
{
    ASTRA_LOG;

    const auto candidatePorts = EnumerateCandidatePorts();
    for (const auto &candidatePort : candidatePorts) {
        const std::string normalizedPort = NormalizePortPath(candidatePort);
        if (!IsValidPort(normalizedPort)) {
            continue;
        }

        if (!MatchesVendorProduct(normalizedPort)) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_activeDevicesMutex);
            if (m_activeDevices.find(normalizedPort) != m_activeDevices.end()) {
                continue;
            }
            m_activeDevices.insert(normalizedPort);
        }

        // Extract vendor/product IDs and interface count from device identity
        uint16_t vendorId = 0;
        uint16_t productId = 0;
        uint8_t numInterfaces = 0;
        const auto identity = ReadPosixIdentity(normalizedPort);
        if (identity.has_value()) {
            if (identity->vendorId.has_value()) {
                vendorId = identity->vendorId.value();
            }
            if (identity->productId.has_value()) {
                productId = identity->productId.value();
            }
            if (identity->numInterfaces.has_value()) {
                const uint32_t n = identity->numInterfaces.value();
                numInterfaces = (n > 255U) ? uint8_t{255} : static_cast<uint8_t>(n);
            }
        }

        std::unique_ptr<USBDevice> usbDevice = std::make_unique<PosixUSBCDCDevice>(normalizedPort,
            vendorId, productId, numInterfaces);
        if (m_deviceAddedCallback) {
            try {
                m_deviceAddedCallback(std::move(usbDevice));
            } catch (...) {
                RemoveActiveDevice(normalizedPort);
            }
        } else {
            RemoveActiveDevice(normalizedPort);
        }
    }
}

std::vector<std::string> PosixUSBCDCTransport::EnumerateCandidatePorts() const
{
    ASTRA_LOG;

    std::vector<std::string> ports;

    const std::filesystem::path devPath("/dev");
    if (!std::filesystem::exists(devPath)) {
        return ports;
    }

    try {
        for (const auto &entry : std::filesystem::directory_iterator(devPath)) {
            const std::string name = entry.path().filename().string();
            const bool isLinuxCdc = (name.rfind("ttyACM", 0) == 0 || name.rfind("ttyUSB", 0) == 0);
            // On macOS only enumerate cu.* (call-out) devices.  The paired tty.*
            // (dial-in) nodes require a DCD/carrier-detect signal before open()
            // succeeds; embedded USB CDC devices never assert DCD, so open()
            // returns ENXIO on those nodes.
            const bool isMacosCdc = (name.rfind("cu.usbmodem", 0) == 0 || name.rfind("cu.usbserial", 0) == 0);

            if (isLinuxCdc || isMacosCdc) {
                ports.push_back(entry.path().string());
            }
        }
    } catch (...) {
        log(ASTRA_LOG_LEVEL_WARNING) << "CDC candidate enumeration failed" << endLog;
    }

    return ports;
}

bool PosixUSBCDCTransport::MatchesVendorProduct(const std::string& portPath) const
{
    ASTRA_LOG;

    if (m_supportedDevices.empty()) {
        return true;
    }

    const auto identity = ReadPosixIdentity(portPath);
    if (!identity.has_value() || !identity->vendorId.has_value() || !identity->productId.has_value()) {
        // If platform metadata is unavailable, do not block detection.
        return true;
    }

    const uint16_t vid = identity->vendorId.value();
    const uint16_t pid = identity->productId.value();
    for (const auto& [v, p] : m_supportedDevices) {
        if (vid == v && pid == p) return true;
    }
    return false;
}

