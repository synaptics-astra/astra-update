// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <iostream>
#include <cstddef>
#include <iomanip>
#include <cstring>
#include <chrono>

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

    ret = libusb_get_config_descriptor(libusb_get_device(m_handle), 0, &m_config);
    if (ret < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to get config descriptor: " << libusb_error_name(ret) << endLog;
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

    ret = libusb_detach_kernel_driver(m_handle, 0);
    if (ret < 0) {
        if (ret == LIBUSB_ERROR_NOT_FOUND || ret == LIBUSB_ERROR_NOT_SUPPORTED) {
            // Since some platforms don't support kernel driver detaching, we'll just log the error and continue
            log(ASTRA_LOG_LEVEL_INFO) << "Failed to detach kernel driver: " << libusb_error_name(ret) << endLog;
        } else {
            // If the error is something else, we'll return an error
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to detach kernel driver: " << libusb_error_name(ret) << endLog;
            return -1;
        }
    }

    ret = libusb_claim_interface(m_handle, m_interfaceNumber);
    if (ret < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to claim interface: " << libusb_error_name(ret) << endLog;
        return -1;
    }

    for (int i = 0; i < m_config->bNumInterfaces; ++i) {
        const libusb_interface &interface = m_config->interface[i];
        for (int j = 0; j < interface.num_altsetting; ++j) {
            const libusb_interface_descriptor &altsetting = interface.altsetting[j];
            log(ASTRA_LOG_LEVEL_INFO) << "Interface Descriptor:" << endLog;
            log(ASTRA_LOG_LEVEL_INFO) << "  bLength: " << static_cast<int>(altsetting.bLength) << endLog;
            log(ASTRA_LOG_LEVEL_INFO) << "  bDescriptorType: " << static_cast<int>(altsetting.bDescriptorType) << endLog;
            log(ASTRA_LOG_LEVEL_INFO) << "  bInterfaceNumber: " << static_cast<int>(altsetting.bInterfaceNumber) << endLog;
            log(ASTRA_LOG_LEVEL_INFO) << "  bAlternateSetting: " << static_cast<int>(altsetting.bAlternateSetting) << endLog;
            log(ASTRA_LOG_LEVEL_INFO) << "  bNumEndpoints: " << static_cast<int>(altsetting.bNumEndpoints) << endLog;
            log(ASTRA_LOG_LEVEL_INFO) << "  bInterfaceClass: " << static_cast<int>(altsetting.bInterfaceClass) << endLog;
            log(ASTRA_LOG_LEVEL_INFO) << "  bInterfaceSubClass: " << static_cast<int>(altsetting.bInterfaceSubClass) << endLog;
            log(ASTRA_LOG_LEVEL_INFO) << "  bInterfaceProtocol: " << static_cast<int>(altsetting.bInterfaceProtocol) << endLog;
            log(ASTRA_LOG_LEVEL_INFO) << "  iInterface: " << static_cast<int>(altsetting.iInterface) << endLog;

            for (int k = 0; k < altsetting.bNumEndpoints; ++k) {
                const libusb_endpoint_descriptor &endpoint = altsetting.endpoint[k];
                log(ASTRA_LOG_LEVEL_INFO) << "Endpoint Descriptor:" << endLog;
                log(ASTRA_LOG_LEVEL_INFO) << "  bLength: " << static_cast<int>(endpoint.bLength) << endLog;
                log(ASTRA_LOG_LEVEL_INFO) << "  bDescriptorType: " << static_cast<int>(endpoint.bDescriptorType) << endLog;
                log(ASTRA_LOG_LEVEL_INFO) << "  bEndpointAddress: " << static_cast<int>(endpoint.bEndpointAddress) << endLog;
                log(ASTRA_LOG_LEVEL_INFO) << "  bmAttributes: " << static_cast<int>(endpoint.bmAttributes) << endLog;
                log(ASTRA_LOG_LEVEL_INFO) << "  wMaxPacketSize: " << endpoint.wMaxPacketSize << endLog;
                log(ASTRA_LOG_LEVEL_INFO) << "  bInterval: " << static_cast<int>(endpoint.bInterval) << endLog;

                if (endpoint.bEndpointAddress & 0x80) {
                    if (endpoint.bmAttributes == 3) {
                        m_interruptInSize = endpoint.wMaxPacketSize;
                        m_interruptInEndpoint = endpoint.bEndpointAddress;
                    } else if (endpoint.bmAttributes == 2) {
                        m_bulkInSize = endpoint.wMaxPacketSize;
                        m_bulkInEndpoint = endpoint.bEndpointAddress;
                    }
                } else {
                    if (endpoint.bmAttributes == 3) {
                        m_interruptOutSize = endpoint.wMaxPacketSize;
                        m_interruptOutEndpoint = endpoint.bEndpointAddress;
                    } else if (endpoint.bmAttributes == 2) {
                        m_bulkOutSize = endpoint.wMaxPacketSize;
                        m_bulkOutEndpoint = endpoint.bEndpointAddress;
                    }
                }

                ret = libusb_clear_halt(m_handle, endpoint.bEndpointAddress);
                if (ret < 0) {
                    log(ASTRA_LOG_LEVEL_ERROR) << "Failed to clear halt on endpoint: " << libusb_error_name(ret) << endLog;
                    return -1;
                }
            }
        }
    }

    m_inputInterruptXfer = libusb_alloc_transfer(0);
    if (!m_inputInterruptXfer) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to allocate input interrupt transfer" << endLog;
        return -1;
    }
    m_outputInterruptXfer = libusb_alloc_transfer(0);
    if (!m_outputInterruptXfer) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to allocate output interrupt transfer" << endLog;
        return -1;
    }

    m_interruptInBuffer = new uint8_t[m_interruptInSize];
    m_interruptOutBuffer = new uint8_t[m_interruptOutSize];

    m_bulkWriteXfer = libusb_alloc_transfer(0);
    if (!m_bulkWriteXfer) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to allocate bulk out transfer" << endLog;
        return -1;
    }

    libusb_fill_interrupt_transfer(m_inputInterruptXfer, m_handle, m_interruptInEndpoint,
        m_interruptInBuffer, m_interruptInSize, HandleTransfer, this, 0);

    return 0;
}

int USBDevice::EnableInterrupts()
{
    ASTRA_LOG;

    // Start callback worker thread
    m_callbackThreadRunning.store(true);
    m_callbackThread = std::thread(&USBDevice::CallbackWorkerThread, this);

    m_running.store(true);
    int ret = libusb_submit_transfer(m_inputInterruptXfer);
    if (ret < 0) {
        m_running.store(false);
        // Stop the callback thread since we failed to start
        m_callbackThreadRunning.store(false);
        m_callbackQueueCV.notify_all();
        if (m_callbackThread.joinable()) {
            m_callbackThread.join();
        }
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to submit input interrupt transfer: " << libusb_error_name(ret) << endLog;
    }

    return ret;
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
