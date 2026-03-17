// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once
#include "device.hpp"
#include "usb_device.hpp"
#include "astra_log.hpp"

#include <libusb-1.0/libusb.h>

class LibUSBDevice : public USBDevice {
public:
    LibUSBDevice(libusb_device *device, const std::string &usbPath, libusb_context *ctx, libusb_device_handle *handle) : USBDevice(usbPath), m_ctx(ctx), m_handle(handle), m_config(nullptr),
        m_inputInterruptXfer(nullptr), m_outputInterruptXfer(nullptr), m_bulkWriteXfer(nullptr),
        m_interruptInEndpoint(0), m_interruptOutEndpoint(0), m_interruptInSize(0), m_interruptOutSize(0),
        m_interruptInBuffer(nullptr), m_interruptOutBuffer(nullptr),
        m_bulkInEndpoint(0), m_bulkOutEndpoint(0), m_bulkInSize(0), m_bulkOutSize(0)
    {
        ASTRA_LOG;

        if (device != nullptr) {
            m_device = device;
            libusb_ref_device(m_device);
        } else {
            m_device = nullptr;
        }
    }
    ~LibUSBDevice() override;

    using USBEvent = USBDevice::USBEvent;

    int Open(std::function<void(USBEvent event, uint8_t *buf, size_t size)> usbEventCallback) override;
    int EnableInterrupts() override;
    void Close() override;

    int Write(uint8_t *data, size_t size, int *transferred) override;

    int WriteInterruptData(const uint8_t *data, size_t size) override;
    uint16_t GetVendorId() const override;
    uint16_t GetProductId() const override;
    uint8_t GetNumInterfaces() const override;

protected:
    libusb_device *m_device;
    libusb_context *m_ctx;
    libusb_device_handle *m_handle;
    libusb_config_descriptor *m_config;
    struct libusb_transfer *m_inputInterruptXfer;
    struct libusb_transfer *m_outputInterruptXfer;
    struct libusb_transfer *m_bulkWriteXfer;

    uint8_t m_interruptInEndpoint;
    uint8_t m_interruptOutEndpoint;
    size_t m_interruptInSize;
    size_t m_interruptOutSize;
    uint8_t *m_interruptInBuffer;
    uint8_t *m_interruptOutBuffer;

    uint8_t m_bulkInEndpoint;
    uint8_t m_bulkOutEndpoint;
    size_t m_bulkInSize;
    size_t m_bulkOutSize;

    std::mutex m_writeCompleteMutex;
    std::condition_variable m_writeCompleteCV;
    std::atomic<bool> m_writeComplete = false;

    int m_bulkTransferTimeout;

    static void LIBUSB_CALL HandleTransfer(struct libusb_transfer *transfer);
};