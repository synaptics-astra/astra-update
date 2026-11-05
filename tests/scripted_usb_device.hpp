// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "usb_device.hpp"

namespace test {

class ScriptedUSBDevice : public USBDevice
{
public:
    struct ReadResult {
        std::vector<uint8_t> bytes;
        int result = 0;
        int transferred = std::numeric_limits<int>::min();
    };

    struct WriteResult {
        int result = 0;
        int transferred = std::numeric_limits<int>::min();
    };

    explicit ScriptedUSBDevice(std::string path = "fake-usb-0") : USBDevice(path) {}

    ~ScriptedUSBDevice() override { StopCallbackWorker(); }

    int Open(std::function<void(USBEvent, uint8_t *, size_t)> callback) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_openCount;
        m_usbEventCallback = std::move(callback);
        return m_openResult;
    }

    void Close() override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_closeCount;
        }
        StopCallbackWorker();
    }

    int Write(uint8_t *data, size_t size, int *transferred) override
    {
        WriteResult result;
        std::function<void(const std::vector<uint8_t> &)> onWrite;
        std::vector<uint8_t> bytes(data, data + size);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_writes.push_back(bytes);
            if (!m_writeResults.empty()) {
                result = m_writeResults.front();
                m_writeResults.pop_front();
            } else {
                result.transferred = static_cast<int>(size);
            }
            if (result.transferred == std::numeric_limits<int>::min() && result.result >= 0) {
                result.transferred = static_cast<int>(size);
            }
            if (result.transferred == std::numeric_limits<int>::min()) {
                result.transferred = 0;
            }
            onWrite = m_onWrite;
        }
        if (transferred != nullptr) {
            *transferred = result.transferred;
        }
        if (onWrite) {
            onWrite(bytes);
        }
        return result.result;
    }

    int ReadBulk(uint8_t *data, size_t size, int *transferred, int timeoutMs = 5000) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_readTimeouts.push_back(timeoutMs);
        if (m_reads.empty()) {
            if (transferred != nullptr) {
                *transferred = 0;
            }
            return -1;
        }
        auto result = std::move(m_reads.front());
        m_reads.pop_front();
        const auto copied = std::min(size, result.bytes.size());
        std::copy_n(result.bytes.begin(), copied, data);
        if (transferred != nullptr) {
            *transferred = result.transferred == std::numeric_limits<int>::min() ?
                static_cast<int>(result.bytes.size()) : result.transferred;
        }
        return result.result;
    }

    int WriteInterruptData(const uint8_t *data, size_t size) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (size == 0) {
            m_interruptWrites.emplace_back();
        } else {
            m_interruptWrites.emplace_back(data, data + size);
        }
        return m_interruptWriteResult;
    }

    void QueueRead(const std::string &response,
        int transferred = std::numeric_limits<int>::min(), int result = 0)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_reads.push_back({std::vector<uint8_t>(response.begin(), response.end()), result, transferred});
    }

    void QueueRead(ReadResult result)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_reads.push_back(std::move(result));
    }

    void QueueWriteResult(WriteResult result)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_writeResults.push_back(result);
    }

    void SetOnWrite(std::function<void(const std::vector<uint8_t> &)> callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_onWrite = std::move(callback);
    }

    void SetOpenResult(int result)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_openResult = result;
    }

    void SetInterruptWriteResult(int result)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_interruptWriteResult = result;
    }

    void Emit(USBEvent event)
    {
        Emit(event, {});
    }

    void Emit(USBEvent event, const std::vector<uint8_t> &data)
    {
        std::function<void(USBEvent, uint8_t *, size_t)> callback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            callback = m_usbEventCallback;
        }
        if (callback) {
            auto mutableData = data;
            callback(event, mutableData.empty() ? nullptr : mutableData.data(), mutableData.size());
        }
    }

    std::vector<std::vector<uint8_t>> Writes() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_writes;
    }

    std::vector<int> ReadTimeouts() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_readTimeouts;
    }

    std::vector<std::vector<uint8_t>> InterruptWrites() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_interruptWrites;
    }

    int OpenCount() const { std::lock_guard<std::mutex> lock(m_mutex); return m_openCount; }
    int CloseCount() const { std::lock_guard<std::mutex> lock(m_mutex); return m_closeCount; }

private:
    mutable std::mutex m_mutex;
    std::deque<ReadResult> m_reads;
    std::deque<WriteResult> m_writeResults;
    std::vector<std::vector<uint8_t>> m_writes;
    std::vector<std::vector<uint8_t>> m_interruptWrites;
    std::vector<int> m_readTimeouts;
    std::function<void(const std::vector<uint8_t> &)> m_onWrite;
    int m_openResult = 0;
    int m_interruptWriteResult = 0;
    int m_openCount = 0;
    int m_closeCount = 0;
};

class CallbackWorkerUSBDevice : public ScriptedUSBDevice
{
public:
    using ScriptedUSBDevice::ScriptedUSBDevice;

    ~CallbackWorkerUSBDevice() override { StopWorker(); }

    void StartWorker() { EnableInterrupts(); }

    void QueueEvent(USBEvent event, const std::vector<uint8_t> &data)
    {
        CallbackEvent callbackEvent;
        callbackEvent.event = event;
        callbackEvent.data = data;
        {
            std::lock_guard<std::mutex> lock(m_callbackQueueMutex);
            m_callbackQueue.push(std::move(callbackEvent));
        }
        m_callbackQueueCV.notify_one();
    }

    void QueueAction(std::function<void()> action) { QueueCallbackAction(std::move(action)); }

    void StopWorker()
    {
        StopCallbackWorker();
    }
};

} // namespace test
