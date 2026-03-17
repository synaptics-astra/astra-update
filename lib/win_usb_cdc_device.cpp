// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "win_usb_cdc_device.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <limits>

#include "astra_log.hpp"

namespace {
constexpr DWORD kWriteWaitTimeoutMs = 15000;
constexpr DWORD kReadPollIntervalMs = 100;
} // namespace

WinUSBCDCDevice::WinUSBCDCDevice(const std::string &usbPath, uint16_t vendorId, uint16_t productId, uint8_t numInterfaces)
    : USBCDCDevice(usbPath, vendorId, productId, numInterfaces), m_handle(INVALID_HANDLE_VALUE)
{
    ASTRA_LOG;
}

WinUSBCDCDevice::~WinUSBCDCDevice()
{
    ASTRA_LOG;
    Close();
}

std::string WinUSBCDCDevice::ToComDevicePath(const std::string& portName) const
{
    std::string normalized = portName;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    static const std::string kComPrefix = "\\\\.\\";
    if (normalized.rfind(kComPrefix, 0) == 0) {
        return portName;
    }

    if (normalized.rfind("COM", 0) == 0) {
        return kComPrefix + portName;
    }

    return portName;
}

int WinUSBCDCDevice::Open(std::function<void(USBEvent event, uint8_t *buf, size_t size)> usbEventCallback)
{
    ASTRA_LOG;

    if (!usbEventCallback) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Missing USB event callback" << endLog;
        return -1;
    }

    std::lock_guard<std::mutex> lock(m_closeMutex);
    if (m_running.load()) {
        return 0;
    }

    m_usbEventCallback = usbEventCallback;

    const std::string comPath = ToComDevicePath(m_usbPath);
    m_handle = CreateFileA(comPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open CDC port " << comPath << ": " << GetLastError() << endLog;
        return -1;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(m_handle, &dcb)) {
        log(ASTRA_LOG_LEVEL_ERROR) << "GetCommState failed: " << GetLastError() << endLog;
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        return -1;
    }

    dcb.BaudRate = kDefaultBaudRate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = TRUE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(m_handle, &dcb)) {
        log(ASTRA_LOG_LEVEL_ERROR) << "SetCommState failed: " << GetLastError() << endLog;
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        return -1;
    }

    if (!EscapeCommFunction(m_handle, SETDTR)) {
        log(ASTRA_LOG_LEVEL_WARNING) << "SETDTR failed: " << GetLastError() << endLog;
    }

    if (!EscapeCommFunction(m_handle, SETRTS)) {
        log(ASTRA_LOG_LEVEL_WARNING) << "SETRTS failed: " << GetLastError() << endLog;
    }

    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 1000;
    timeouts.WriteTotalTimeoutMultiplier = 10;

    if (!SetCommTimeouts(m_handle, &timeouts)) {
        log(ASTRA_LOG_LEVEL_ERROR) << "SetCommTimeouts failed: " << GetLastError() << endLog;
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        return -1;
    }

    SetupComm(m_handle, 64 * 1024, 64 * 1024);
    PurgeComm(m_handle, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

    m_shutdown.store(false);
    m_running.store(true);
    m_readerThread = std::thread(&WinUSBCDCDevice::ReaderThread, this);

    return 0;
}

int WinUSBCDCDevice::EnableInterrupts()
{
    ASTRA_LOG;
    return USBCDCDevice::EnableInterrupts();
}

void WinUSBCDCDevice::Close()
{
    ASTRA_LOG;

    m_writeCompleteCV.notify_all();

    {
        std::lock_guard<std::mutex> lock(m_closeMutex);
        if (!m_shutdown.exchange(true)) {
            m_running.store(false);

            if (m_handle != INVALID_HANDLE_VALUE) {
                CancelIoEx(m_handle, nullptr);
                CloseHandle(m_handle);
                m_handle = INVALID_HANDLE_VALUE;
            }
        }
    }

    if (m_readerThread.joinable()) {
        m_readerThread.join();
    }

    StopCallbackWorker();
}

int WinUSBCDCDevice::Write(uint8_t *data, size_t size, int *transferred)
{
    ASTRA_LOG;

    if (!m_running.load() || m_handle == INVALID_HANDLE_VALUE) {
        if (transferred) {
            *transferred = 0;
        }
        return -1;
    }

    if (size > static_cast<size_t>((std::numeric_limits<DWORD>::max)())) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Write size exceeds DWORD limit on " << m_usbPath << ": " << size << endLog;
        if (transferred) {
            *transferred = 0;
        }
        return -1;
    }

    auto queueWriteFailure = [this, transferred](DWORD error) {
        CallbackEvent event;
        if (error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_INVALID_HANDLE || error == ERROR_GEN_FAILURE) {
            event.event = USB_DEVICE_EVENT_NO_DEVICE;
            m_running.store(false);
        } else {
            event.event = USB_DEVICE_EVENT_TRANSFER_ERROR;
        }

        {
            std::lock_guard<std::mutex> lock(m_callbackQueueMutex);
            m_callbackQueue.push(std::move(event));
        }
        m_callbackQueueCV.notify_one();

        if (transferred) {
            *transferred = 0;
        }
    };

    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        const DWORD error = GetLastError();
        log(ASTRA_LOG_LEVEL_ERROR) << "CreateEvent failed for write on " << m_usbPath << ": " << error << endLog;
        queueWriteFailure(error);
        return -1;
    }

    DWORD bytesWritten = 0;
    BOOL result = WriteFile(m_handle, data, static_cast<DWORD>(size), &bytesWritten, &overlapped);
    if (!result) {
        DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            log(ASTRA_LOG_LEVEL_ERROR) << "WriteFile failed on " << m_usbPath << ": " << error << endLog;
            CloseHandle(overlapped.hEvent);
            queueWriteFailure(error);
            return -1;
        }

        const DWORD waitStatus = WaitForSingleObject(overlapped.hEvent, kWriteWaitTimeoutMs);
        if (waitStatus == WAIT_TIMEOUT) {
            CancelIoEx(m_handle, &overlapped);
            log(ASTRA_LOG_LEVEL_ERROR) << "WriteFile timed out on " << m_usbPath << " after " << kWriteWaitTimeoutMs
                                       << "ms for " << size << " bytes" << endLog;
            CloseHandle(overlapped.hEvent);
            queueWriteFailure(ERROR_TIMEOUT);
            return -1;
        }

        if (waitStatus != WAIT_OBJECT_0) {
            error = GetLastError();
            log(ASTRA_LOG_LEVEL_ERROR) << "WaitForSingleObject failed during write on " << m_usbPath
                                       << ": " << error << endLog;
            CancelIoEx(m_handle, &overlapped);
            CloseHandle(overlapped.hEvent);
            queueWriteFailure(error);
            return -1;
        }

        result = GetOverlappedResult(m_handle, &overlapped, &bytesWritten, FALSE);
        if (!result) {
            error = GetLastError();
            log(ASTRA_LOG_LEVEL_ERROR) << "GetOverlappedResult failed on write for " << m_usbPath
                                       << ": " << error << endLog;
            CloseHandle(overlapped.hEvent);
            queueWriteFailure(error);
            return -1;
        }
    }

    CloseHandle(overlapped.hEvent);

    if (transferred) {
        *transferred = static_cast<int>(bytesWritten);
    }

    return 0;
}

void WinUSBCDCDevice::ReaderThread()
{
    ASTRA_LOG;

    std::array<uint8_t, 4096> buffer = {};
    while (m_running.load()) {
        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (overlapped.hEvent == nullptr) {
            const DWORD error = GetLastError();
            log(ASTRA_LOG_LEVEL_ERROR) << "CreateEvent failed for read on " << m_usbPath << ": " << error << endLog;
            break;
        }

        DWORD bytesRead = 0;
        BOOL result = ReadFile(m_handle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, &overlapped);

        if (!result && GetLastError() == ERROR_IO_PENDING) {
            DWORD waitStatus = WAIT_TIMEOUT;
            while (waitStatus == WAIT_TIMEOUT && m_running.load()) {
                waitStatus = WaitForSingleObject(overlapped.hEvent, kReadPollIntervalMs);
            }

            if (waitStatus == WAIT_OBJECT_0) {
                result = GetOverlappedResult(m_handle, &overlapped, &bytesRead, FALSE);
            } else {
                CancelIoEx(m_handle, &overlapped);
                CloseHandle(overlapped.hEvent);
                if (!m_running.load()) {
                    break;
                }

                const DWORD error = (waitStatus == WAIT_FAILED) ? GetLastError() : ERROR_OPERATION_ABORTED;
                CallbackEvent event;
                event.event = USB_DEVICE_EVENT_TRANSFER_ERROR;
                {
                    std::lock_guard<std::mutex> lock(m_callbackQueueMutex);
                    m_callbackQueue.push(std::move(event));
                }
                m_callbackQueueCV.notify_one();
                m_running.store(false);
                log(ASTRA_LOG_LEVEL_ERROR) << "Read wait failed on " << m_usbPath << ": " << error << endLog;
                break;
            }
        }

        if (!result) {
            const DWORD error = GetLastError();
            CloseHandle(overlapped.hEvent);
            if (!m_running.load() || error == ERROR_OPERATION_ABORTED) {
                break;
            }

            CallbackEvent event;
            if (error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_INVALID_HANDLE || error == ERROR_GEN_FAILURE) {
                event.event = USB_DEVICE_EVENT_NO_DEVICE;
            } else {
                event.event = USB_DEVICE_EVENT_TRANSFER_ERROR;
            }

            {
                std::lock_guard<std::mutex> lock(m_callbackQueueMutex);
                m_callbackQueue.push(std::move(event));
            }
            m_callbackQueueCV.notify_one();
            m_running.store(false);
            break;
        }

        CloseHandle(overlapped.hEvent);

        if (bytesRead > 0) {
            CallbackEvent event;
            event.event = USB_DEVICE_EVENT_INTERRUPT;
            event.data.assign(buffer.begin(), buffer.begin() + bytesRead);
            {
                std::lock_guard<std::mutex> lock(m_callbackQueueMutex);
                m_callbackQueue.push(std::move(event));
            }
            m_callbackQueueCV.notify_one();
        }
    }
}

uint16_t WinUSBCDCDevice::GetVendorId() const
{
    return m_vendorId;
}

uint16_t WinUSBCDCDevice::GetProductId() const
{
    return m_productId;
}

uint8_t WinUSBCDCDevice::GetNumInterfaces() const
{
    return m_numInterfaces;
}
