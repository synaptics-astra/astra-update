// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <iostream>
#include <cstddef>
#include <iomanip>
#include <cstring>

#include "usb_device.hpp"
#include "astra_log.hpp"

USBDevice::USBDevice(libusb_device *device, const std::string &usbPath, libusb_context *ctx)
{
    ASTRA_LOG;

    m_device = libusb_ref_device(device);
    m_ctx = ctx;
    m_handle = nullptr;
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

    if (m_handle) {
        return 0;
    }

    if (!usbEventCallback) {
        return 1;
    }

    m_usbEventCallback = usbEventCallback;

    int ret = libusb_open(m_device, &m_handle);
    if (ret < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open USB device: " << libusb_error_name(ret) << endLog;
        return -1;
    }

    if (m_handle == nullptr) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open USB device" << endLog;
        return -1;
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

    int ret = libusb_submit_transfer(m_inputInterruptXfer);
    if (ret < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to submit input interrupt transfer: " << libusb_error_name(ret) << endLog;
    }

    m_running.store(true);

    return ret;
}

void USBDevice::Close()
{
    ASTRA_LOG;

    std::lock_guard<std::mutex> lock(m_closeMutex);
    if (!m_shutdown.exchange(true))
    {
        m_running.store(false);
        if (m_inputInterruptXfer) {
            struct timeval tv = { 1, 0 };
            libusb_cancel_transfer(m_inputInterruptXfer);
            libusb_handle_events_timeout_completed(m_ctx, &tv, nullptr);
            libusb_free_transfer(m_inputInterruptXfer);
            m_inputInterruptXfer = nullptr;
        }

        if (m_outputInterruptXfer) {
            struct timeval tv = { 1, 0 };
            libusb_cancel_transfer(m_outputInterruptXfer);
            libusb_handle_events_timeout_completed(m_ctx, &tv, nullptr);
            libusb_free_transfer(m_outputInterruptXfer);
            m_outputInterruptXfer = nullptr;
        }

        if (m_bulkWriteXfer) {
            struct timeval tv = { 1, 0 };
            libusb_cancel_transfer(m_bulkWriteXfer);
            libusb_handle_events_timeout_completed(m_ctx, &tv, nullptr);
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

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (transfer->type == LIBUSB_TRANSFER_TYPE_BULK) {
            if (transfer->endpoint == device->m_bulkOutEndpoint) {
                device->m_actualBytesWritten = transfer->actual_length;
                device->m_writeComplete.store(true);
                device->m_writeCompleteCV.notify_one();
            }
        } else if (transfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
            if (transfer->endpoint == device->m_interruptInEndpoint) {
                device->m_usbEventCallback(USB_DEVICE_EVENT_INTERRUPT, transfer->buffer, transfer->actual_length);
            }
            resubmit = true;
        }
    } else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        device->m_running.store(false);
        log(ASTRA_LOG_LEVEL_INFO) << "Device is no longer there during transfer: " << libusb_error_name(transfer->status) << endLog;
        device->m_usbEventCallback(USB_DEVICE_EVENT_NO_DEVICE, nullptr, 0);
    } else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        device->m_running.store(false);
        log(ASTRA_LOG_LEVEL_DEBUG) << "Input transfer cancelled" << endLog;
        device->m_usbEventCallback(USB_DEVICE_EVENT_TRANSFER_CANCELED, nullptr, 0);
    } else if (transfer->status == LIBUSB_TRANSFER_STALL) {
        log(ASTRA_LOG_LEVEL_WARNING) << "Endpoint stalled, clearing halt" << endLog;
        int ret = libusb_clear_halt(device->m_handle, transfer->endpoint);
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to clear halt on endpoint: " << libusb_error_name(ret) << endLog;
            if (ret == LIBUSB_ERROR_NO_DEVICE) {
                device->m_running.store(false);
                device->m_usbEventCallback(USB_DEVICE_EVENT_NO_DEVICE, nullptr, 0);
            } else {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to clear halt on endpoint: " << libusb_error_name(ret) << endLog;
            }
        } else {
            log(ASTRA_LOG_LEVEL_INFO) << "Halt cleared, retrying transfer" << endLog;
            resubmit = true;
        }
    } else {
        log(ASTRA_LOG_LEVEL_ERROR) << "Transfer failed: " << libusb_error_name(transfer->status) << endLog;
        device->m_usbEventCallback(USB_DEVICE_EVENT_TRANSFER_ERROR, nullptr, 0);
    }

    if (resubmit && device->m_running.load()) {
        log(ASTRA_LOG_LEVEL_DEBUG) << "Resubmitting transfer" << endLog;
        int ret = libusb_submit_transfer(transfer);
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to submit transfer: " << libusb_error_name(ret) << endLog;
            device->m_usbEventCallback(USB_DEVICE_EVENT_TRANSFER_ERROR, nullptr, 0);
        }
    }
}
