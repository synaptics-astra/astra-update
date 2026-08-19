// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "usb_device.hpp"
#include "usb_transport.hpp"

class USBCDCTransport : public USBTransport {
public:
    explicit USBCDCTransport(bool usbDebug) : USBTransport(usbDebug)
    {}
    ~USBCDCTransport() override;

    int Init(std::vector<USBVendorProductId> vendorProductIds, const std::string filterPorts,
        std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback) override;
    void Shutdown() override;

    bool BlockDeviceEnumeration() override { return true; }
    void UnblockDeviceEnumeration() override {}
    void RemoveActiveDevice(const std::string& usbPath) override;

    virtual void StartDeviceMonitor();

protected:
    void DeviceMonitorThread();
    virtual void ProcessPendingDevices() = 0;
    bool IsValidPort(const std::string& portPath) const;
    virtual std::string NormalizePortPath(const std::string& portPath) const;

    // Stop and join any platform-specific monitor thread started by
    // StartDeviceMonitor() (udev on Linux, IOKit on macOS).  Called from
    // Shutdown() so the monitor cannot deliver a device arrival after the
    // transport has been shut down.  Must be idempotent: the platform
    // destructor calls it again as a safety net.
    virtual void StopPlatformMonitor() {}

    std::set<std::string> m_activeDevices;
    std::mutex m_activeDevicesMutex;

    std::atomic<bool> m_hasPendingDevices{false};
    std::condition_variable m_pendingDevicesCV;
    std::mutex m_pendingDevicesMutex;
};
