// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <functional>
#include <string>

#include "usb_cdc_device.hpp"

class PosixUSBCDCDevice : public USBCDCDevice {
public:
    PosixUSBCDCDevice(const std::string &usbPath, uint16_t vendorId = 0, uint16_t productId = 0, uint8_t numInterfaces = 0);
    ~PosixUSBCDCDevice() override;

    int Open(std::function<void(USBEvent event, uint8_t *buf, size_t size)> usbEventCallback) override;
    void Close() override;
    int Write(uint8_t *data, size_t size, int *transferred) override;
    uint16_t GetVendorId() const override;
    uint16_t GetProductId() const override;
    uint8_t GetNumInterfaces() const override;

private:
    void ReaderThread();

    int m_fd;
};
