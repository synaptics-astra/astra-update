// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include "usb_cdc_transport.hpp"

#if defined(PLATFORM_LINUX)
#include <thread>
struct udev_device;
#endif

class PosixUSBCDCTransport : public USBCDCTransport {
public:
    explicit PosixUSBCDCTransport(bool usbDebug)
        : USBCDCTransport(usbDebug)
    {}
    ~PosixUSBCDCTransport() override;

private:
    void ProcessPendingDevices() override;
    std::vector<std::string> EnumerateCandidatePorts() const;
    bool MatchesVendorProduct(const std::string& portPath) const;

#if defined(PLATFORM_LINUX)
    void StartDeviceMonitor() override;
    void UdevMonitorThread();
    void ProcessUdevDevice(udev_device* dev);

    std::thread m_udevThread;
    int m_wakeupPipe[2] = {-1, -1};
#endif
};
