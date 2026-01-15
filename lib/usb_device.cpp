// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <iostream>
#include <cstddef>
#include <iomanip>
#include <cstring>
#include <chrono>
#include <thread>

#include "usb_device.hpp"
#include "astra_log.hpp"

USBDevice::USBDevice(libusb_device *device, const std::string &usbPath, libusb_context *ctx, libusb_device_handle *handle)
{
    ASTRA_LOG;

    m_device = libusb_ref_device(device);
    m_ctx = ctx;
    m_handle = handle;
    m_config = nullptr;
    m_running.store(false);
    m_interruptInEndpoint = 0;
    m_interruptOutEndpoint = 0;
    m_interfaceNumber = 0;
    m_interruptInSize = 0;
    m_interruptOutSize = 0;
    m_interruptInBuffer = nullptr;
    m_interruptOutBuffer = nullptr;
    m_inputInterruptXfer = nullptr;
    m_outputInterruptXfer = nullptr;
    m_bulkWriteXfer = nullptr;
    m_bulkInEndpoint = 0;
    m_bulkOutEndpoint = 0;
    m_bulkInSize = 0;
    m_bulkOutSize = 0;
    m_bulkTransferTimeout = 1000;
    m_usbPath = usbPath;
}

USBDevice::~USBDevice()
{
    ASTRA_LOG;

    Close();
}

int USBDevice::Open(std::function<void(USBEvent event, uint8_t *buf, size_t size)> usbEventCallback)
{
    ASTRA_LOG;

    int ret = 0;

    if (m_handle && m_config) {
        log(ASTRA_LOG_LEVEL_INFO) << "USB device is already open" << endLog;
        return 0;
    }

    if (!usbEventCallback) {
        return 1;
    }

    m_usbEventCallback = usbEventCallback;

    if (m_handle == nullptr) {
        ret = libusb_open(m_device, &m_handle);
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open USB device: " << libusb_error_name(ret) << endLog;
            return -1;
        }

        if (m_handle == nullptr) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open USB device" << endLog;
            return -1;
        }
    }

    // Detach kernel driver early - this can help with config descriptor access
    // LIBUSB_ERROR_NOT_FOUND means no driver attached, LIBUSB_ERROR_INVALID_PARAM can occur
    // if the device is in a bad state (e.g., 0 interfaces) - let the retry loop handle it
    if (libusb_has_capability(LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER)) {
        ret = libusb_detach_kernel_driver(m_handle, 0);
        if (ret < 0 && ret != LIBUSB_ERROR_NOT_FOUND && ret != LIBUSB_ERROR_INVALID_PARAM) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to detach kernel driver: " << libusb_error_name(ret) << endLog;
            return -1;
        }
    }

    // Get config descriptor with retry logic - device may be in transitional state
    const int maxRetries = 4;
    const int retryDelayMs = 100;
    bool configValid = false;

    for (int attempt = 0; attempt < maxRetries; ++attempt) {
        if (m_config) {
            libusb_free_config_descriptor(m_config);
            m_config = nullptr;
        }

        ret = libusb_get_active_config_descriptor(libusb_get_device(m_handle), &m_config);
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_WARNING) << "Failed to get config descriptor (attempt "
                << (attempt + 1) << "/" << maxRetries << "): " << libusb_error_name(ret) << endLog;
        } else if (m_config->bNumInterfaces == 0) {
            log(ASTRA_LOG_LEVEL_WARNING) << "Config descriptor has 0 interfaces (attempt "
                << (attempt + 1) << "/" << maxRetries << ")" << endLog;
        } else {
            configValid = true;
            break;
        }

        if (attempt < maxRetries - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
        }
    }

    // If retries failed, reset the device to unstick it and return error
    // The reset will trigger re-enumeration and the transport will create a new device instance
    if (!configValid) {
        log(ASTRA_LOG_LEVEL_WARNING) << "Config descriptor invalid after " << maxRetries
            << " attempts, resetting device" << endLog;

        int resetRet = libusb_reset_device(m_handle);
        if (resetRet < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Device reset failed: " << libusb_error_name(resetRet) << endLog;
        } else {
            log(ASTRA_LOG_LEVEL_INFO) << "Device reset successful, waiting for re-enumeration" << endLog;
        }

        return -1;
    }

    log(ASTRA_LOG_LEVEL_INFO) << "Configuration Descriptor:" << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  bLength: " << static_cast<int>(m_config->bLength) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  bDescriptorType: " << static_cast<int>(m_config->bDescriptorType) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  wTotalLength: " << m_config->wTotalLength << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  bNumInterfaces: " << static_cast<int>(m_config->bNumInterfaces) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  bConfigurationValue: " << static_cast<int>(m_config->bConfigurationValue) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  iConfiguration: " << static_cast<int>(m_config->iConfiguration) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  bmAttributes: " << static_cast<int>(m_config->bmAttributes) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  MaxPower: " << static_cast<int>(m_config->MaxPower) << endLog;

    unsigned char serialNumber[256];
    libusb_device_descriptor desc;
    ret = libusb_get_device_descriptor(m_device, &desc);
    if (ret < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to get device descriptor: " << libusb_error_name(ret) << endLog;
        return -1;
    }

    if (desc.iSerialNumber != 0) {
        ret = libusb_get_string_descriptor_ascii(m_handle, desc.iSerialNumber, serialNumber, sizeof(serialNumber));
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to get serial number: " << libusb_error_name(ret) << endLog;
        } else {
            m_serialNumber = std::string(serialNumber, serialNumber + ret);
            log(ASTRA_LOG_LEVEL_INFO) << "Serial number: " << m_serialNumber << endLog;
        }
    }

    log(ASTRA_LOG_LEVEL_DEBUG) << "USB Path: " << m_usbPath << endLog;

    ret = libusb_claim_interface(m_handle, m_interfaceNumber);
    if (ret < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to claim interface: " << libusb_error_name(ret) << endLog;
        return -1;
    }

    // Find the claimed interface descriptor (use alternate setting 0)
    const libusb_interface_descriptor *ifaceDesc = nullptr;
    for (int i = 0; i < m_config->bNumInterfaces; ++i) {
        const libusb_interface &interface = m_config->interface[i];
        for (int j = 0; j < interface.num_altsetting; ++j) {
            const libusb_interface_descriptor &altsetting = interface.altsetting[j];
            if (altsetting.bInterfaceNumber == m_interfaceNumber && altsetting.bAlternateSetting == 0) {
                ifaceDesc = &altsetting;
                break;
            }
        }
        if (ifaceDesc) break;
    }

    if (!ifaceDesc) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Interface " << m_interfaceNumber << " not found in config descriptor" << endLog;
        return -1;
    }

    log(ASTRA_LOG_LEVEL_INFO) << "Interface Descriptor:" << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  bLength: " << static_cast<int>(ifaceDesc->bLength) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  bDescriptorType: " << static_cast<int>(ifaceDesc->bDescriptorType) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  bInterfaceNumber: " << static_cast<int>(ifaceDesc->bInterfaceNumber) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  bAlternateSetting: " << static_cast<int>(ifaceDesc->bAlternateSetting) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  bNumEndpoints: " << static_cast<int>(ifaceDesc->bNumEndpoints) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  bInterfaceClass: " << static_cast<int>(ifaceDesc->bInterfaceClass) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  bInterfaceSubClass: " << static_cast<int>(ifaceDesc->bInterfaceSubClass) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  bInterfaceProtocol: " << static_cast<int>(ifaceDesc->bInterfaceProtocol) << endLog;
    log(ASTRA_LOG_LEVEL_INFO) << "  iInterface: " << static_cast<int>(ifaceDesc->iInterface) << endLog;

    // Discover endpoints from the claimed interface
    for (int k = 0; k < ifaceDesc->bNumEndpoints; ++k) {
        const libusb_endpoint_descriptor &endpoint = ifaceDesc->endpoint[k];
        log(ASTRA_LOG_LEVEL_INFO) << "Endpoint Descriptor:" << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bLength: " << static_cast<int>(endpoint.bLength) << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bDescriptorType: " << static_cast<int>(endpoint.bDescriptorType) << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bEndpointAddress: " << static_cast<int>(endpoint.bEndpointAddress) << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bmAttributes: " << static_cast<int>(endpoint.bmAttributes) << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  wMaxPacketSize: " << endpoint.wMaxPacketSize << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bInterval: " << static_cast<int>(endpoint.bInterval) << endLog;

        uint8_t transferType = endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
        bool isIn = (endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;

        if (isIn) {
            if (transferType == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                m_interruptInSize = endpoint.wMaxPacketSize;
                m_interruptInEndpoint = endpoint.bEndpointAddress;
            } else if (transferType == LIBUSB_TRANSFER_TYPE_BULK) {
                m_bulkInSize = endpoint.wMaxPacketSize;
                m_bulkInEndpoint = endpoint.bEndpointAddress;
            }
        } else {
            if (transferType == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                m_interruptOutSize = endpoint.wMaxPacketSize;
                m_interruptOutEndpoint = endpoint.bEndpointAddress;
            } else if (transferType == LIBUSB_TRANSFER_TYPE_BULK) {
                m_bulkOutSize = endpoint.wMaxPacketSize;
                m_bulkOutEndpoint = endpoint.bEndpointAddress;
            }
        }

        // Clear any halt condition on the endpoint
        ret = libusb_clear_halt(m_handle, endpoint.bEndpointAddress);
        if (ret < 0 && ret != LIBUSB_ERROR_NOT_FOUND) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to clear halt on endpoint 0x"
                << std::hex << static_cast<int>(endpoint.bEndpointAddress) << std::dec
                << ": " << libusb_error_name(ret) << endLog;
            return -1;
        }
    }

    // Validate that required endpoints were found
    if (m_interruptInEndpoint == 0 || m_interruptInSize == 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Interrupt IN endpoint not found" << endLog;
        return -1;
    }
    if (m_interruptOutEndpoint == 0 || m_interruptOutSize == 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Interrupt OUT endpoint not found" << endLog;
        return -1;
    }
    if (m_bulkOutEndpoint == 0 || m_bulkOutSize == 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Bulk OUT endpoint not found" << endLog;
        return -1;
    }

    log(ASTRA_LOG_LEVEL_DEBUG) << "Discovered endpoints - Interrupt IN: 0x" << std::hex
        << static_cast<int>(m_interruptInEndpoint) << ", Interrupt OUT: 0x"
        << static_cast<int>(m_interruptOutEndpoint) << ", Bulk OUT: 0x"
        << static_cast<int>(m_bulkOutEndpoint) << std::dec << endLog;

    // Allocate transfers and buffers with cleanup on failure
    m_inputInterruptXfer = libusb_alloc_transfer(0);
    if (!m_inputInterruptXfer) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to allocate input interrupt transfer" << endLog;
        return -1;
    }

    m_outputInterruptXfer = libusb_alloc_transfer(0);
    if (!m_outputInterruptXfer) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to allocate output interrupt transfer" << endLog;
        libusb_free_transfer(m_inputInterruptXfer);
        m_inputInterruptXfer = nullptr;
        return -1;
    }

    m_bulkWriteXfer = libusb_alloc_transfer(0);
    if (!m_bulkWriteXfer) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to allocate bulk out transfer" << endLog;
        libusb_free_transfer(m_inputInterruptXfer);
        m_inputInterruptXfer = nullptr;
        libusb_free_transfer(m_outputInterruptXfer);
        m_outputInterruptXfer = nullptr;
        return -1;
    }

    m_interruptInBuffer = new uint8_t[m_interruptInSize];
    m_interruptOutBuffer = new uint8_t[m_interruptOutSize];

    libusb_fill_interrupt_transfer(m_inputInterruptXfer, m_handle, m_interruptInEndpoint,
        m_interruptInBuffer, m_interruptInSize, HandleTransfer, this, 0);

    // Submit interrupt transfer immediately to avoid losing interrupts
    // Interrupts will be queued until EnableInterrupts() starts the callback worker thread
    m_running.store(true);
    ret = libusb_submit_transfer(m_inputInterruptXfer);
    if (ret < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to submit input interrupt transfer: " << libusb_error_name(ret) << endLog;
        m_running.store(false);
        delete[] m_interruptInBuffer;
        m_interruptInBuffer = nullptr;
        delete[] m_interruptOutBuffer;
        m_interruptOutBuffer = nullptr;
        libusb_free_transfer(m_inputInterruptXfer);
        m_inputInterruptXfer = nullptr;
        libusb_free_transfer(m_outputInterruptXfer);
        m_outputInterruptXfer = nullptr;
        libusb_free_transfer(m_bulkWriteXfer);
        m_bulkWriteXfer = nullptr;
        return ret;
    }

    log(ASTRA_LOG_LEVEL_DEBUG) << "Interrupt transfer submitted, interrupts will queue" << endLog;
    return 0;
}

int USBDevice::EnableInterrupts()
{
    ASTRA_LOG;

    // Start callback worker thread to process queued interrupts
    // Interrupt transfer was already submitted in Open(), so interrupts are already queuing
    m_callbackThreadRunning.store(true);
    m_callbackThread = std::thread(&USBDevice::CallbackWorkerThread, this);

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

void USBDevice::Close()
{
    ASTRA_LOG;

    m_writeCompleteCV.notify_all();

    // Stop callback thread
    if (m_callbackThreadRunning.exchange(false)) {
        m_callbackQueueCV.notify_all();
        if (m_callbackThread.joinable()) {
            m_callbackThread.join();
        }
    }

    std::lock_guard<std::mutex> lock(m_closeMutex);
    if (!m_shutdown.exchange(true))
    {
        m_running.store(false);

        // Cancel all transfers and track which ones need to complete
        bool waitForInputInterrupt = false;
        bool waitForOutputInterrupt = false;
        bool waitForBulkWrite = false;

        if (m_inputInterruptXfer) {
            int ret = libusb_cancel_transfer(m_inputInterruptXfer);
            if (ret == 0) {
                waitForInputInterrupt = true;
            }
        }

        if (m_outputInterruptXfer) {
            int ret = libusb_cancel_transfer(m_outputInterruptXfer);
            if (ret == 0) {
                waitForOutputInterrupt = true;
            }
        }

        if (m_bulkWriteXfer) {
            int ret = libusb_cancel_transfer(m_bulkWriteXfer);
            if (ret == 0) {
                waitForBulkWrite = true;
            }
        }

        // Wait for cancellation callbacks to complete (with timeout for safety)
        // DeviceMonitorThread will process these callbacks and notify us
        if (waitForInputInterrupt || waitForOutputInterrupt || waitForBulkWrite) {
            std::unique_lock<std::mutex> lock(m_cancellationMutex);
            m_cancellationCV.wait_for(lock, std::chrono::milliseconds(500), [&]() {
                bool allCancelled = true;
                if (waitForInputInterrupt && !m_inputInterruptCancelled.load()) {
                    allCancelled = false;
                }
                if (waitForOutputInterrupt && !m_outputInterruptCancelled.load()) {
                    allCancelled = false;
                }
                if (waitForBulkWrite && !m_bulkWriteCancelled.load()) {
                    allCancelled = false;
                }
                return allCancelled;
            });
        }

        // Free transfers
        if (m_inputInterruptXfer) {
            libusb_free_transfer(m_inputInterruptXfer);
            m_inputInterruptXfer = nullptr;
        }

        if (m_outputInterruptXfer) {
            libusb_free_transfer(m_outputInterruptXfer);
            m_outputInterruptXfer = nullptr;
        }

        if (m_bulkWriteXfer) {
            libusb_free_transfer(m_bulkWriteXfer);
            m_bulkWriteXfer = nullptr;
        }

        delete[] m_interruptInBuffer;
        m_interruptInBuffer = nullptr;

        delete[] m_interruptOutBuffer;
        m_interruptOutBuffer = nullptr;

        if (m_handle) {
            libusb_release_interface(m_handle, m_interfaceNumber);
            libusb_close(m_handle);
            m_handle = nullptr;
        }

        if (m_config) {
            libusb_free_config_descriptor(m_config);
            m_config = nullptr;
        }

        libusb_unref_device(m_device);
    }
}

int USBDevice::Write(uint8_t *data, size_t size, int *transferred)
{
    ASTRA_LOG;

    if (!m_running.load()) {
        return -1;
    }

    m_actualBytesWritten = 0;

    log(ASTRA_LOG_LEVEL_DEBUG) << "Writing to USB device" << endLog;
    log(ASTRA_LOG_LEVEL_DEBUG) << "  Bulk Out Endpoint: " << static_cast<int>(m_bulkOutEndpoint) << endLog;
    log(ASTRA_LOG_LEVEL_DEBUG) << "  Length: " << size << endLog;
    log(ASTRA_LOG_LEVEL_DEBUG) << "  Data: ";
    for (size_t i = 0; i < 16 && i < size; ++i) {
        log << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << " ";
    }
    log << std::dec << endLog;

    libusb_fill_bulk_transfer(m_bulkWriteXfer, m_handle, m_bulkOutEndpoint, data, size, HandleTransfer, this, m_bulkTransferTimeout);

    for (;;) {
        int ret = libusb_submit_transfer(m_bulkWriteXfer);
        if (ret < 0) {
            if (ret == LIBUSB_ERROR_TIMEOUT) {
                log(ASTRA_LOG_LEVEL_ERROR) << "USB transfer timed out" << endLog;
            } else if (ret == LIBUSB_ERROR_NO_DEVICE) {
                log(ASTRA_LOG_LEVEL_ERROR) << "USB device is no longer available" << endLog;
                m_running.store(false);
            } else if (ret == LIBUSB_ERROR_PIPE) {
                log(ASTRA_LOG_LEVEL_WARNING) << "Endpoint halted, clearing halt" << endLog;
                ret = libusb_clear_halt(m_handle, m_bulkOutEndpoint);
                if (ret < 0) {
                    log(ASTRA_LOG_LEVEL_ERROR) << "Failed to clear halt on endpoint: " << libusb_error_name(ret) << endLog;
                } else {
                    log(ASTRA_LOG_LEVEL_INFO) << "Halt cleared, retrying transfer" << endLog;
                    continue;
                }
            } else {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to write to USB device: " << libusb_error_name(ret) << endLog;
            }
            return -1;
        }
        break;
    }

    std::unique_lock<std::mutex> lock(m_writeCompleteMutex);
    m_writeCompleteCV.wait(lock, [this] {
        if (m_writeComplete.load()) {
            m_writeComplete.store(false);
            return true;
        }
        return false;
    });

    *transferred = m_actualBytesWritten;

    log(ASTRA_LOG_LEVEL_DEBUG) << "Write Complete: bytes written: " << m_actualBytesWritten << endLog;

    return 0;
}

int USBDevice::WriteInterruptData(const uint8_t *data, size_t size)
{
    ASTRA_LOG;

    if (!m_running.load()) {
        return -1;
    }

    log(ASTRA_LOG_LEVEL_DEBUG) << "Sending interrupt out transfer" << endLog;
    log(ASTRA_LOG_LEVEL_DEBUG) << "  Interrupt Out Endpoint: " << static_cast<int>(m_interruptOutEndpoint) << endLog;
    log(ASTRA_LOG_LEVEL_DEBUG) << "  Length: " << size << endLog;
    log(ASTRA_LOG_LEVEL_DEBUG) << "  Data: ";
    for (size_t i = 0; i < 16 && i < size; ++i) {
        log << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << " ";
    }
    log << std::dec << endLog;

    std::memcpy(m_interruptOutBuffer, data, size);

    libusb_fill_interrupt_transfer(m_outputInterruptXfer, m_handle, m_interruptOutEndpoint,
        m_interruptOutBuffer, size, nullptr, nullptr, 0);

    int ret = libusb_submit_transfer(m_outputInterruptXfer);
    if (ret < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to submit output interrupt transfer: " << libusb_error_name(ret) << endLog;
        return 1;
    }

    return 0;
}

void USBDevice::HandleTransfer(struct libusb_transfer *transfer)
{
    ASTRA_LOG;

    USBDevice *device = static_cast<USBDevice*>(transfer->user_data);

    bool resubmit = false;

    auto queueEvent = [device](USBEvent event, uint8_t *buf, size_t size) {
        CallbackEvent cbEvent;
        cbEvent.event = event;
        if (buf && size > 0) {
            cbEvent.data.assign(buf, buf + size);
        }
        std::lock_guard<std::mutex> lock(device->m_callbackQueueMutex);
        device->m_callbackQueue.push(std::move(cbEvent));
        device->m_callbackQueueCV.notify_one();
    };

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (transfer->type == LIBUSB_TRANSFER_TYPE_BULK) {
            if (transfer->endpoint == device->m_bulkOutEndpoint) {
                {
                    std::lock_guard<std::mutex> lock(device->m_writeCompleteMutex);
                    device->m_actualBytesWritten = transfer->actual_length;
                    device->m_writeComplete.store(true);
                }
                device->m_writeCompleteCV.notify_one();
            }
        } else if (transfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
            if (transfer->endpoint == device->m_interruptInEndpoint) {
                log(ASTRA_LOG_LEVEL_DEBUG) << "Received Interrupt" << endLog;
                queueEvent(USB_DEVICE_EVENT_INTERRUPT, transfer->buffer, transfer->actual_length);
            }
            resubmit = true;
        }
    } else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        device->m_running.store(false);
        log(ASTRA_LOG_LEVEL_INFO) << "Device is no longer there during transfer: " << libusb_error_name(transfer->status) << endLog;

        // Set cancellation flag so Close() doesn't timeout waiting
        {
            std::lock_guard<std::mutex> lock(device->m_cancellationMutex);
            if (transfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                if (transfer->endpoint == device->m_interruptInEndpoint) {
                    device->m_inputInterruptCancelled.store(true);
                } else if (transfer->endpoint == device->m_interruptOutEndpoint) {
                    device->m_outputInterruptCancelled.store(true);
                }
            } else if (transfer->type == LIBUSB_TRANSFER_TYPE_BULK) {
                if (transfer->endpoint == device->m_bulkOutEndpoint) {
                    device->m_bulkWriteCancelled.store(true);
                }
            }
        }
        device->m_cancellationCV.notify_one();

        queueEvent(USB_DEVICE_EVENT_NO_DEVICE, nullptr, 0);
    } else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        device->m_running.store(false);
        log(ASTRA_LOG_LEVEL_DEBUG) << "Transfer cancelled" << endLog;

        // Set cancellation flag for the specific transfer and notify
        {
            std::lock_guard<std::mutex> lock(device->m_cancellationMutex);
            if (transfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                if (transfer->endpoint == device->m_interruptInEndpoint) {
                    device->m_inputInterruptCancelled.store(true);
                } else if (transfer->endpoint == device->m_interruptOutEndpoint) {
                    device->m_outputInterruptCancelled.store(true);
                }
            } else if (transfer->type == LIBUSB_TRANSFER_TYPE_BULK) {
                if (transfer->endpoint == device->m_bulkOutEndpoint) {
                    device->m_bulkWriteCancelled.store(true);
                }
            }
        }
        device->m_cancellationCV.notify_one();

        queueEvent(USB_DEVICE_EVENT_TRANSFER_CANCELED, nullptr, 0);
    } else if (transfer->status == LIBUSB_TRANSFER_STALL) {
        log(ASTRA_LOG_LEVEL_WARNING) << "Endpoint stalled, clearing halt" << endLog;
        int ret = libusb_clear_halt(device->m_handle, transfer->endpoint);
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to clear halt on endpoint: " << libusb_error_name(ret) << endLog;
            if (ret == LIBUSB_ERROR_NO_DEVICE) {
                device->m_running.store(false);
                queueEvent(USB_DEVICE_EVENT_NO_DEVICE, nullptr, 0);
            } else {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to clear halt on endpoint: " << libusb_error_name(ret) << endLog;
            }
        } else {
            log(ASTRA_LOG_LEVEL_INFO) << "Halt cleared, retrying transfer" << endLog;
            resubmit = true;
        }
    } else {
        log(ASTRA_LOG_LEVEL_ERROR) << "Transfer failed: " << libusb_error_name(transfer->status) << endLog;
        queueEvent(USB_DEVICE_EVENT_TRANSFER_ERROR, nullptr, 0);
    }

    if (resubmit && device->m_running.load()) {
        log(ASTRA_LOG_LEVEL_DEBUG) << "Resubmitting transfer" << endLog;
        int ret = libusb_submit_transfer(transfer);
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to submit transfer: " << libusb_error_name(ret) << endLog;
            queueEvent(USB_DEVICE_EVENT_TRANSFER_ERROR, nullptr, 0);
        }
    }
}
