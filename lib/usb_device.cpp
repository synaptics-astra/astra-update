// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <iostream>
#include <cstddef>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>

#include "usb_device.hpp"
#include "astra_log.hpp"

USBDevice::~USBDevice()
{
    ASTRA_LOG;
}

int USBDevice::EnableInterrupts()
{
    ASTRA_LOG;

    // Start callback worker thread to process queued interrupts
    // Interrupt transfer was already submitted in Open(), so interrupts are already queuing
    m_callbackThreadRunning.store(true);
    m_callbackThread = std::thread([this]() { this->CallbackWorkerThread(); });

    log(ASTRA_LOG_LEVEL_DEBUG) << "Callback worker thread started, processing queued interrupts" << endLog;

    return 0;
}

void USBDevice::CallbackWorkerThread()
{
    ASTRA_LOG;

    while (m_callbackThreadRunning.load()) {
        CallbackEvent event;
        {
            std::unique_lock<std::mutex> lock(m_callbackQueueMutex);
            m_callbackQueueCV.wait(lock, [this] {
                return !m_callbackQueue.empty() || !m_callbackThreadRunning.load();
            });

            if (!m_callbackThreadRunning.load() && m_callbackQueue.empty()) {
                break;
            }

            if (!m_callbackQueue.empty()) {
                event = std::move(m_callbackQueue.front());
                m_callbackQueue.pop();
            } else {
                continue;
            }
        }

        // Call the callback outside the lock to avoid blocking the queue
        if (m_usbEventCallback) {
            m_usbEventCallback(event.event, event.data.empty() ? nullptr : event.data.data(), event.data.size());
        }
    }
}
