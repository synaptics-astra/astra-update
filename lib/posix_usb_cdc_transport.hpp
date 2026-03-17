// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include "usb_cdc_transport.hpp"

class PosixUSBCDCTransport : public USBCDCTransport {
public:
    explicit PosixUSBCDCTransport(bool usbDebug)
        : USBCDCTransport(usbDebug)
    {}
    ~PosixUSBCDCTransport() override = default;

private:
    void ProcessPendingDevices() override;
    std::vector<std::string> EnumerateCandidatePorts() const;
    bool MatchesVendorProduct(const std::string& portPath) const;
};
