// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <windows.h>
#include <dbt.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "usb_cdc_transport.hpp"

class WinUSBCDCTransport : public USBCDCTransport {
public:
    explicit WinUSBCDCTransport(bool usbDebug)
        : USBCDCTransport(usbDebug), m_hWnd(nullptr), m_hDevNotify(nullptr)
    {}
    ~WinUSBCDCTransport() override;

    int Init(std::vector<USBVendorProductId> vendorProductIds, const std::string filterPorts,
        std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback) override;
    void Shutdown() override;

    void RemoveActiveDevice(const std::string& usbPath) override;

private:
    struct EnumeratedPort {
        std::string m_port;
        uint16_t m_vendorId{0};
        uint16_t m_productId{0};
        uint8_t m_numInterfaces{0};
    };

    void RunHotplugHandler();
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    void OnDeviceChange();
    void DeviceEnumerationWorker();
    void ProcessPendingDevices() override;
    std::vector<EnumeratedPort> EnumerateMatchingPorts() const;
    std::string NormalizePortPath(const std::string& portPath) const override;

    static std::string ExtractComPortFromFriendlyName(const std::string& friendlyName);
    static bool ExtractVidPid(const std::string& hardwareId, uint16_t &vendorId, uint16_t &productId);

    HWND m_hWnd;
    HDEVNOTIFY m_hDevNotify;

    std::thread m_hotplugThread;
    std::thread m_deviceEnumerationThread;
    std::atomic<bool> m_enumerationThreadRunning{false};
    std::atomic<bool> m_hasPendingDevices{false};
    std::mutex m_pendingDevicesMutex;
    std::condition_variable m_pendingDevicesCV;

    std::set<std::string> m_activeDevices;
    std::mutex m_activeDevicesMutex;
};
