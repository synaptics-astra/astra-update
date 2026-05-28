// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "usb_device.hpp"
#include "astra_log.hpp"

class USBCDCDevice : public USBDevice {
public:
    static constexpr uint32_t kDefaultBaudRate = 230400;

    explicit USBCDCDevice(const std::string &usbPath, uint16_t vendorId = 0, uint16_t productId = 0, uint8_t numInterfaces = 0)
        : USBDevice(usbPath), m_vendorId(vendorId), m_productId(productId), m_numInterfaces(numInterfaces)
    {
        ASTRA_LOG;
    }
    ~USBCDCDevice() override;

    virtual int Open(std::function<void(USBEvent event, uint8_t *buf, size_t size)> usbEventCallback) override = 0;
    int EnableInterrupts() override;
    virtual void Close() override = 0;

    virtual int Write(uint8_t *data, size_t size, int *transferred) override = 0;
    int WriteInterruptData(const uint8_t *data, size_t size) override;

protected:
    void StopCallbackWorker();

    std::thread m_readerThread;
    uint16_t m_vendorId;
    uint16_t m_productId;
    uint8_t m_numInterfaces;
};