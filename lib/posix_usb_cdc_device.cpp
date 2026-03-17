// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "posix_usb_cdc_device.hpp"

#include <array>
#include <chrono>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include "astra_log.hpp"

PosixUSBCDCDevice::PosixUSBCDCDevice(const std::string &usbPath, uint16_t vendorId, uint16_t productId, uint8_t numInterfaces)
    : USBCDCDevice(usbPath, vendorId, productId, numInterfaces), m_fd(-1)
{
    ASTRA_LOG;
}

PosixUSBCDCDevice::~PosixUSBCDCDevice()
{
    ASTRA_LOG;
    Close();
}

int PosixUSBCDCDevice::Open(std::function<void(USBEvent event, uint8_t *buf, size_t size)> usbEventCallback)
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
    m_fd = open(m_usbPath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (m_fd < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open serial device: " << m_usbPath << " errno: " << errno << endLog;
        return -1;
    }

    termios tty = {};
    if (tcgetattr(m_fd, &tty) != 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "tcgetattr failed for " << m_usbPath << " errno: " << errno << endLog;
        close(m_fd);
        m_fd = -1;
        return -1;
    }

    cfmakeraw(&tty);
    // B230400 is provided by some platform termios headers; fall back to B115200
    // when that baud-rate macro is unavailable so this builds across POSIX targets.
#ifdef B230400
    cfsetispeed(&tty, B230400);
    cfsetospeed(&tty, B230400);
#else
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    log(ASTRA_LOG_LEVEL_WARNING) << "B230400 unavailable, using B115200 for CDC serial port" << endLog;
#endif
    tty.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(m_fd, TCSANOW, &tty) != 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "tcsetattr failed for " << m_usbPath << " errno: " << errno << endLog;
        close(m_fd);
        m_fd = -1;
        return -1;
    }

    int flags = fcntl(m_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(m_fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    m_shutdown.store(false);
    m_running.store(true);
    m_readerThread = std::thread(&PosixUSBCDCDevice::ReaderThread, this);

    return 0;
}

void PosixUSBCDCDevice::Close()
{
    ASTRA_LOG;

    m_writeCompleteCV.notify_all();

    {
        std::lock_guard<std::mutex> lock(m_closeMutex);
        if (!m_shutdown.exchange(true)) {
            m_running.store(false);

            if (m_fd >= 0) {
                close(m_fd);
                m_fd = -1;
            }
        }
    }

    if (m_readerThread.joinable()) {
        m_readerThread.join();
    }

    StopCallbackWorker();
}

int PosixUSBCDCDevice::Write(uint8_t *data, size_t size, int *transferred)
{
    ASTRA_LOG;

    if (!m_running.load() || m_fd < 0) {
        if (transferred) {
            *transferred = 0;
        }
        return -1;
    }

    ssize_t bytesWritten = write(m_fd, data, size);
    if (bytesWritten < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Serial write failed on " << m_usbPath << " errno: " << errno << endLog;

        if (errno == ENODEV || errno == EIO || errno == EBADF) {
            CallbackEvent event;
            event.event = USB_DEVICE_EVENT_NO_DEVICE;
            {
                std::lock_guard<std::mutex> lock(m_callbackQueueMutex);
                m_callbackQueue.push(std::move(event));
            }
            m_callbackQueueCV.notify_one();
            m_running.store(false);
        }

        if (transferred) {
            *transferred = 0;
        }
        return -1;
    }

    if (transferred) {
        *transferred = static_cast<int>(bytesWritten);
    }

    return 0;
}

void PosixUSBCDCDevice::ReaderThread()
{
    ASTRA_LOG;

    std::array<uint8_t, 4096> buffer = {};
    while (m_running.load()) {
        ssize_t bytesRead = read(m_fd, buffer.data(), buffer.size());
        if (bytesRead > 0) {
            CallbackEvent event;
            event.event = USB_DEVICE_EVENT_INTERRUPT;
            event.data.assign(buffer.begin(), buffer.begin() + bytesRead);
            {
                std::lock_guard<std::mutex> lock(m_callbackQueueMutex);
                m_callbackQueue.push(std::move(event));
            }
            m_callbackQueueCV.notify_one();
            continue;
        }

        if (bytesRead == 0) {
            continue;
        }

        if (errno == EINTR) {
            continue;
        }

        if ((errno == EAGAIN || errno == EWOULDBLOCK) && m_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (!m_running.load()) {
            break;
        }

        CallbackEvent event;
        if (errno == ENODEV || errno == EIO || errno == EBADF) {
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
}

uint16_t PosixUSBCDCDevice::GetVendorId() const
{
    return m_vendorId;
}

uint16_t PosixUSBCDCDevice::GetProductId() const
{
    return m_productId;
}

uint8_t PosixUSBCDCDevice::GetNumInterfaces() const
{
    return m_numInterfaces;
}
