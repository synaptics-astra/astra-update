// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "usb_cdc_device.hpp"

USBCDCDevice::~USBCDCDevice()
{
    ASTRA_LOG;
}

int USBCDCDevice::EnableInterrupts()
{
    ASTRA_LOG;
    return USBDevice::EnableInterrupts();
}

int USBCDCDevice::WriteInterruptData(const uint8_t *data, size_t size)
{
    ASTRA_LOG;

    (void)data;
    (void)size;

    // CDC transport has no supported interrupt-out console path.
    log(ASTRA_LOG_LEVEL_WARNING) << "WriteInterruptData is unsupported for CDC devices" << endLog;
    return -1;
}

void USBCDCDevice::StopCallbackWorker()
{
    ASTRA_LOG;

    if (m_callbackThreadRunning.exchange(false)) {
        m_callbackQueueCV.notify_all();
        if (m_callbackThread.joinable()) {
            m_callbackThread.join();
        }
    }
}
