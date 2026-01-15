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
#include <libusb-1.0/libusb.h>
#include <mutex>

#include "usb_device.hpp"

class USBTransport {
public:
    USBTransport(bool usbDebug) : m_usbDebug{usbDebug}, m_ctx{nullptr}, m_running{false}
    {}
    virtual ~USBTransport();

    virtual int Init(uint16_t vendorId, uint16_t productId, const std::string filterPorts,
        std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback);
    virtual void Shutdown();

    // Block/unblock device enumeration during critical boot sequences
    // Default implementations are no-ops for non-Windows platforms
    virtual void BlockDeviceEnumeration() {}
    virtual void UnblockDeviceEnumeration() {}

    void StartDeviceMonitor();

protected:
    bool m_usbDebug;
    libusb_context *m_ctx;
    libusb_hotplug_callback_handle m_callbackHandle;
    std::function<void(std::unique_ptr<USBDevice>)> m_deviceAddedCallback;
    std::thread m_deviceMonitorThread;
    std::atomic<bool> m_running;
    std::mutex m_shutdownMutex;
    uint16_t m_vendorId;
    uint16_t m_productId;
    std::vector<std::string> m_filterPorts;

    void DeviceMonitorThread();
    std::vector<std::string> ParseFilterPortString(const std::string& filterPorts);
    std::string ConstructUSBPath(libusb_device *device);
    bool IsValidPort(libusb_device *device, const std::string &portString);

    static int LIBUSB_CALL HotplugEventCallback(libusb_context *ctx, libusb_device *device,
                                                libusb_hotplug_event event, void *user_data);
};