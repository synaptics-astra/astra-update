// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <iostream>
#include <iomanip>
#include <libusb-1.0/libusb.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <sstream>

#include "usb_transport.hpp"
#include "astra_log.hpp"

USBTransport::~USBTransport()
{
    ASTRA_LOG;
    Shutdown();

    if (m_ctx) {
        libusb_exit(m_ctx);
        m_ctx = nullptr;
    }
}

void USBTransport::DeviceMonitorThread()
{
    ASTRA_LOG;

    int ret;

    while (m_running.load()) {
        struct timeval tv = { 1, 0 };
        ret = libusb_handle_events_timeout_completed(m_ctx, &tv, nullptr);
        if (ret < 0) {
            if (ret == LIBUSB_ERROR_INTERRUPTED) {
                log(ASTRA_LOG_LEVEL_DEBUG) << "libusb_handle_events_timeout_completed interrupted" << endLog;
                continue;
            }
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to handle events: " << libusb_error_name(ret) << endLog;
            break;
        }
    }
}

// Windows overrides this function in win_usb_transport.cpp. Add code which needs to run on Windows to that function as well.
int USBTransport::Init(uint16_t vendorId, uint16_t productId, const std::string filterPorts,
    std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback)
{
    ASTRA_LOG;

    m_vendorId = vendorId;
    m_productId = productId;

    m_filterPorts = ParseFilterPortString(filterPorts);

    int ret = libusb_init(&m_ctx);
    if (ret < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to initialize libusb: " << libusb_error_name(ret) << endLog;
    }

    if (m_usbDebug) {
        libusb_set_option(m_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG);
    }

    m_deviceAddedCallback = deviceAddedCallback;

    m_running.store(true);
    if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        log(ASTRA_LOG_LEVEL_DEBUG) << "Hotplug is supported" << endLog;

        ret = libusb_hotplug_register_callback(m_ctx,
                                                LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                                LIBUSB_HOTPLUG_ENUMERATE,
                                                vendorId,
                                                productId,
                                                LIBUSB_HOTPLUG_MATCH_ANY,
                                                HotplugEventCallback,
                                                this,
                                                &m_callbackHandle);
        if (ret != LIBUSB_SUCCESS) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to register hotplug callback: " << libusb_error_name(ret) << endLog;
        }

    } else {
        log(ASTRA_LOG_LEVEL_DEBUG) << "Hotplug is NOT supported" << endLog;
    }

    StartDeviceMonitor();

    return ret;
}

void USBTransport::Shutdown()
{
    ASTRA_LOG;

    std::lock_guard<std::mutex> lock(m_shutdownMutex);
    if (m_running.exchange(false)) {
        if (m_callbackHandle) {
            libusb_hotplug_deregister_callback(m_ctx, m_callbackHandle);
            m_callbackHandle = 0;
        }

        libusb_interrupt_event_handler(m_ctx);
        if (m_deviceMonitorThread.joinable()) {
            m_deviceMonitorThread.join();
        }
    }
}

void USBTransport::StartDeviceMonitor()
{
    ASTRA_LOG;

    m_deviceMonitorThread = std::thread(&USBTransport::DeviceMonitorThread, this);
}

std::vector<std::string> USBTransport::ParseFilterPortString(const std::string & filterPorts)
{
    ASTRA_LOG;

    std::vector<std::string> filterList;

    if (!filterPorts.empty()) {
        size_t start = 0;
        size_t end = 0;
        while ((end = filterPorts.find(',', start)) != std::string::npos) {
            std::string port = filterPorts.substr(start, end - start);
            if (!port.empty()) {
                filterList.push_back(port);
                log(ASTRA_LOG_LEVEL_DEBUG) << "Adding filter port: " << port << endLog;
            }
            start = end + 1;
        }
        std::string lastPort = filterPorts.substr(start);
        if (!lastPort.empty()) {
            filterList.push_back(lastPort);
            log(ASTRA_LOG_LEVEL_DEBUG) << "Adding filter port: " << lastPort << endLog;
        }
    }

    return filterList;
}

std::string USBTransport::ConstructUSBPath(libusb_device *device)
{
    ASTRA_LOG;

    std::stringstream portStream;
    uint8_t portNumbers[8];
    uint8_t bus = libusb_get_bus_number(device);
    uint8_t port = libusb_get_port_number(device);
    int numElementsInPath = libusb_get_port_numbers(device, portNumbers, 8);
    portStream << static_cast<int>(bus) << "-";
    if (numElementsInPath > 0) {
        portStream << static_cast<int>(portNumbers[0]);
        for (int i = 1; i < numElementsInPath; ++i) {
            portStream << "." << static_cast<int>(portNumbers[i]);
        }
    }

    log(ASTRA_LOG_LEVEL_DEBUG) << "USB Path: " << portStream.str() << endLog;
    return portStream.str();
}

bool USBTransport::IsValidPort(libusb_device *device, const std::string &devicePath)
{
    ASTRA_LOG;

    if (m_filterPorts.empty()) {
        return true;
    }

    for (const auto& port : m_filterPorts) {
        if (devicePath.rfind(port, 0) == 0) {
            return true;
        }
    }

    return false;
}

int LIBUSB_CALL USBTransport::HotplugEventCallback(libusb_context *ctx, libusb_device *device,
                                                libusb_hotplug_event event, void *user_data)
{
    ASTRA_LOG;

    USBTransport *transport = static_cast<USBTransport*>(user_data);

    libusb_device_descriptor desc;
    int ret = libusb_get_device_descriptor(device, &desc);
    if (ret < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to get device descriptor" << endLog;
        return 1;
    }

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        log(ASTRA_LOG_LEVEL_INFO) << "Device arrived: vid: 0x" << std::hex << std::uppercase << desc.idVendor << ", pid: 0x" << desc.idProduct << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "Device matches image" << endLog;

        log(ASTRA_LOG_LEVEL_INFO) << "Device Descriptor:" << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bLength: " << static_cast<int>(desc.bLength) << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bDescriptorType: " << static_cast<int>(desc.bDescriptorType) << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bcdUSB: " << desc.bcdUSB << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bDeviceClass: " << static_cast<int>(desc.bDeviceClass) << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bDeviceSubClass: " << static_cast<int>(desc.bDeviceSubClass) << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bDeviceProtocol: " << static_cast<int>(desc.bDeviceProtocol) << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bMaxPacketSize0: " << static_cast<int>(desc.bMaxPacketSize0) << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  idVendor: 0x" << std::hex << std::setw(4) << std::setfill('0') << desc.idVendor << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  idProduct: 0x" << std::hex << std::setw(4) << std::setfill('0') << desc.idProduct << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bcdDevice: " << desc.bcdDevice << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  iManufacturer: " << static_cast<int>(desc.iManufacturer) << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  iProduct: " << static_cast<int>(desc.iProduct) << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  iSerialNumber: " << static_cast<int>(desc.iSerialNumber) << endLog;
        log(ASTRA_LOG_LEVEL_INFO) << "  bNumConfigurations: " << static_cast<int>(desc.bNumConfigurations) << endLog;

        std::string usbPath = transport->ConstructUSBPath(device);
        if (!transport->IsValidPort(device, usbPath)) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Device is not on a port we are monitoring" << endLog;
            return 0;
        }

        std::unique_ptr<USBDevice> usbDevice = std::make_unique<USBDevice>(device, usbPath, transport->m_ctx);
        if (transport->m_deviceAddedCallback) {
            try {
                transport->m_deviceAddedCallback(std::move(usbDevice));
            } catch (const std::bad_function_call& e) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Error: " << e.what() << endLog;
                return 1;
            }
        } else {
            log(ASTRA_LOG_LEVEL_ERROR) << "No device added callback" << endLog;
        }
    }

    return 0;
}