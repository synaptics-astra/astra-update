// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once
#include "usb_transport.hpp"
#include "libusb_device.hpp"

#include <libusb-1.0/libusb.h>

class LibUSBTransport : public USBTransport{
public:
    LibUSBTransport(bool usbDebug) : USBTransport(usbDebug), m_ctx{nullptr}
    {}
    virtual ~LibUSBTransport();

    virtual int Init(std::vector<USBVendorProductId> vendorProductIds, const std::string filterPorts,
        std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback);
    virtual void Shutdown();

    // Block/unblock device enumeration during critical boot sequences
    // Default implementations are no-ops for non-Windows platforms
    virtual bool BlockDeviceEnumeration() { return true; }
    virtual void UnblockDeviceEnumeration() {}
    virtual void RemoveActiveDevice(const std::string& usbPath) {}

    void StartDeviceMonitor();

protected:
    libusb_context *m_ctx;
    libusb_hotplug_callback_handle m_callbackHandle;
    std::function<void(std::unique_ptr<USBDevice>)> m_deviceAddedCallback;

    void DeviceMonitorThread();
    std::string ConstructUSBPath(libusb_device *device);
    bool IsValidPort(libusb_device *device, const std::string &portString);

    static int LIBUSB_CALL HotplugEventCallback(libusb_context *ctx, libusb_device *device,
                                                libusb_hotplug_event event, void *user_data);
};
