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

#include "device.hpp"
#include "astra_log.hpp"

class USBDevice : public Device {
public:
    USBDevice(const std::string &usbPath) : m_usbPath(usbPath), m_interfaceNumber(0)
    {
        ASTRA_LOG;
    }
    ~USBDevice() override;

    enum USBEvent {
        USB_DEVICE_EVENT_NO_DEVICE,
        USB_DEVICE_EVENT_TRANSFER_CANCELED,
        USB_DEVICE_EVENT_TRANSFER_ERROR,
        USB_DEVICE_EVENT_INTERRUPT,
    };

    virtual int Open(std::function<void(USBEvent event, uint8_t *buf, size_t size)> usbEventCallback) = 0;
    virtual int EnableInterrupts();
    void Close() override = 0;

    std::string &GetUSBPath() { return m_usbPath; }
    virtual uint16_t GetVendorId() const { return 0; }
    virtual uint16_t GetProductId() const { return 0; }
    virtual uint8_t GetNumInterfaces() const { return 0; }

    int Write(uint8_t *data, size_t size, int *transferred) override = 0;

    virtual int WriteInterruptData(const uint8_t *data, size_t size) = 0;

protected:
    int m_actualBytesWritten;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_shutdown{false};
    std::mutex m_closeMutex;
    std::string m_serialNumber;
    std::string m_usbPath;
    int m_interfaceNumber;

    std::mutex m_writeCompleteMutex;
    std::condition_variable m_writeCompleteCV;
    std::atomic<bool> m_writeComplete = false;

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

    // Transfer cancellation tracking
    std::atomic<bool> m_inputInterruptCancelled{false};
    std::atomic<bool> m_outputInterruptCancelled{false};
    std::atomic<bool> m_bulkWriteCancelled{false};
    std::mutex m_cancellationMutex;
    std::condition_variable m_cancellationCV;

    void CallbackWorkerThread();
};