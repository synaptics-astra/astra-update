// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once
#include <cstdint>
#include <cstddef>
#include <thread>
#include <atomic>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>
#include <libusb-1.0/libusb.h>

#include "device.hpp"

class USBDevice : public Device {
public:
    USBDevice(libusb_device *device, const std::string &usbPath, libusb_context *ctx);
    ~USBDevice();

    enum USBEvent {
        USB_DEVICE_EVENT_NO_DEVICE,
        USB_DEVICE_EVENT_TRANSFER_CANCELED,
        USB_DEVICE_EVENT_TRANSFER_ERROR,
        USB_DEVICE_EVENT_INTERRUPT,
    };

    int Open(std::function<void(USBEvent event, uint8_t *buf, size_t size)> usbEventCallback);
    int EnableInterrupts();
    void Close() override;

    std::string &GetUSBPath() { return m_usbPath; }

    int Write(uint8_t *data, size_t size, int *transferred) override;

    int WriteInterruptData(const uint8_t *data, size_t size);

private:
    libusb_device *m_device;
    libusb_context *m_ctx;
    libusb_device_handle *m_handle;
    libusb_config_descriptor *m_config;
    struct libusb_transfer *m_inputInterruptXfer;
    struct libusb_transfer *m_outputInterruptXfer;
    struct libusb_transfer *m_bulkWriteXfer;
    int m_actualBytesWritten;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_shutdown{false};
    std::mutex m_closeMutex;
    std::string m_serialNumber;
    std::string m_usbPath;
    int m_interfaceNumber;

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

    std::function<void(USBEvent event, uint8_t *buf, size_t size)> m_usbEventCallback;

    // Async callback processing
    struct CallbackEvent {
        USBEvent event;
        std::vector<uint8_t> data;
    };
    std::queue<CallbackEvent> m_callbackQueue;
    std::mutex m_callbackQueueMutex;
    std::condition_variable m_callbackQueueCV;
    std::thread m_callbackThread;
    std::atomic<bool> m_callbackThreadRunning{false};

    void CallbackWorkerThread();

    static void LIBUSB_CALL HandleTransfer(struct libusb_transfer *transfer);
};