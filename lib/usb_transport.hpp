// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once
#include <vector>
#include <tuple>
#include <cstdint>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <utility>

#include "usb_device.hpp"

using USBVendorProductId = std::pair<uint16_t, uint16_t>;

class USBTransport {
public:
    USBTransport(bool usbDebug) : m_usbDebug{usbDebug}, m_running{false}
    {}
    virtual ~USBTransport();

    virtual int Init(std::vector<USBVendorProductId> vendorProductIds, const std::string filterPorts,
        std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback) = 0;
    virtual int Init(uint16_t vendorId, uint16_t productId, const std::string filterPorts,
        std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback)
    {
        return Init(std::vector<USBVendorProductId>{{vendorId, productId}}, filterPorts, deviceAddedCallback);
    }
    virtual void Shutdown() = 0;

    // Configure before Init(). Return true to consume an arrival without opening
    // the device. Runs on the enumeration/event thread and must not block.
    void SetDeviceObservedCallback(
        std::function<bool(uint16_t, uint16_t, const std::string &)> callback)
    {
        m_deviceObservedCallback = std::move(callback);
    }

    // Block/unblock device enumeration during critical boot sequences
    // Default implementations are no-ops for non-Windows platforms
    virtual bool BlockDeviceEnumeration() { return true; }
    virtual void UnblockDeviceEnumeration() {}
    virtual void RemoveActiveDevice(const std::string& usbPath) {}

    void StartDeviceMonitor();

protected:
    bool m_usbDebug;
    std::function<void(std::unique_ptr<USBDevice>)> m_deviceAddedCallback;
    std::thread m_deviceMonitorThread;
    std::atomic<bool> m_running;
    std::mutex m_shutdownMutex;
    std::vector<USBVendorProductId> m_supportedDevices;
    std::vector<std::string> m_filterPorts;

    std::function<bool(uint16_t, uint16_t, const std::string &)> m_deviceObservedCallback;
    bool NotifyDeviceObserved(uint16_t vendorId, uint16_t productId, const std::string &usbPath);
    std::vector<std::string> ParseFilterPortString(const std::string& filterPorts);
};
