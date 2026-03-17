// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "usb_cdc_transport.hpp"

#include <chrono>
#include <filesystem>

#include "astra_log.hpp"

USBCDCTransport::~USBCDCTransport()
{
    ASTRA_LOG;
    Shutdown();
}

int USBCDCTransport::Init(std::vector<USBVendorProductId> vendorProductIds, const std::string filterPorts,
    std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback)
{
    ASTRA_LOG;

    if (!deviceAddedCallback) {
        log(ASTRA_LOG_LEVEL_ERROR) << "CDC transport init failed: missing callback" << endLog;
        return -1;
    }

    m_supportedDevices = vendorProductIds;
    m_filterPorts = ParseFilterPortString(filterPorts);
    for (auto &port : m_filterPorts) {
        port = NormalizePortPath(port);
    }

    m_deviceAddedCallback = deviceAddedCallback;

    m_running.store(true);
    m_hasPendingDevices.store(true);
    StartDeviceMonitor();
    m_pendingDevicesCV.notify_one();

    return 0;
}

void USBCDCTransport::Shutdown()
{
    ASTRA_LOG;

    std::lock_guard<std::mutex> lock(m_shutdownMutex);
    if (m_running.exchange(false)) {
        m_pendingDevicesCV.notify_all();
        if (m_deviceMonitorThread.joinable()) {
            m_deviceMonitorThread.join();
        }

        std::lock_guard<std::mutex> activeLock(m_activeDevicesMutex);
        m_activeDevices.clear();
    }
}

void USBCDCTransport::StartDeviceMonitor()
{
    ASTRA_LOG;

    m_deviceMonitorThread = std::thread(&USBCDCTransport::DeviceMonitorThread, this);
}

void USBCDCTransport::DeviceMonitorThread()
{
    ASTRA_LOG;

    while (m_running.load()) {
        {
            std::unique_lock<std::mutex> lock(m_pendingDevicesMutex);
            m_pendingDevicesCV.wait_for(lock, std::chrono::milliseconds(500), [this] {
                return m_hasPendingDevices.load() || !m_running.load();
            });

            if (!m_running.load()) {
                break;
            }

            m_hasPendingDevices.store(false);
        }

        ProcessPendingDevices();

        // Polling fallback for platforms without hotplug callbacks.
        m_hasPendingDevices.store(true);
    }
}

bool USBCDCTransport::IsValidPort(const std::string& portPath) const
{
    ASTRA_LOG;

    if (m_filterPorts.empty()) {
        return true;
    }

    const std::string normalizedPort = NormalizePortPath(portPath);
    const std::string shortName = std::filesystem::path(normalizedPort).filename().string();

    for (const auto &filterPort : m_filterPorts) {
        const std::string normalizedFilter = NormalizePortPath(filterPort);

        if (normalizedPort == normalizedFilter || shortName == normalizedFilter) {
            return true;
        }

        if (normalizedPort.rfind(normalizedFilter, 0) == 0) {
            return true;
        }
    }

    return false;
}

void USBCDCTransport::RemoveActiveDevice(const std::string& usbPath)
{
    ASTRA_LOG;

    std::lock_guard<std::mutex> lock(m_activeDevicesMutex);
    m_activeDevices.erase(NormalizePortPath(usbPath));
}

std::string USBCDCTransport::NormalizePortPath(const std::string& portPath) const
{
    return portPath;
}
