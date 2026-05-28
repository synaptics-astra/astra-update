// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include "usb_cdc_transport.hpp"
#include <memory>

#if defined(PLATFORM_LINUX)
struct udev_device;
#elif defined(PLATFORM_MACOS)
#include <IOKit/IOKitLib.h>   // io_object_t, io_iterator_t
#endif

class PosixUSBCDCTransport : public USBCDCTransport {
public:
    explicit PosixUSBCDCTransport(bool usbDebug);
    ~PosixUSBCDCTransport() override;

private:
    void ProcessPendingDevices() override;
    void StartDeviceMonitor() override;
    std::vector<std::string> EnumerateCandidatePorts() const;
    bool MatchesVendorProduct(const std::string& portPath) const;

#if defined(PLATFORM_LINUX)
    void UdevMonitorThread();
    void ProcessUdevDevice(udev_device *dev);
#elif defined(PLATFORM_MACOS)
    void IOKitMonitorThread();
    void ProcessIOKitService(io_object_t service);
    static void IOKitDeviceAdded(void *ctx, io_iterator_t iter);
    static void IOKitDeviceRemoved(void *ctx, io_iterator_t iter);
#endif

    // Platform-specific state is held in Impl (defined in the platform .cpp).
    // Only one platform file is compiled per build, so there is no ODR issue.
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
