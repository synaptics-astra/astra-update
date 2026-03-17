// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <windows.h>
#include <dbt.h>
#include <thread>
#include <memory>
#include <set>
#include <string>

#include "libusb_transport.hpp"

class WinLibUSBTransport : public LibUSBTransport {
public:
    WinLibUSBTransport(bool usbDebug) : LibUSBTransport(usbDebug), m_hWnd(nullptr), m_hDevNotify(nullptr), m_hCriticalSectionMutex(nullptr) {};
    ~WinLibUSBTransport() override;
    int Init(std::vector<USBVendorProductId> vendorProductIds, const std::string filterPorts, std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback) override;
    void Shutdown() override;

    bool BlockDeviceEnumeration() override;
    void UnblockDeviceEnumeration() override;
    void RemoveActiveDevice(const std::string& usbPath) override;

private:
    void RunHotplugHandler();
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    void OnDeviceArrived();
    void DeviceEnumerationWorker();
    void ProcessPendingDevices();

    std::function<void(std::unique_ptr<USBDevice>)> m_deviceAddedCallback;
    HWND m_hWnd;
    HDEVNOTIFY m_hDevNotify;
    HANDLE m_hCriticalSectionMutex;  // Serializes critical boot section across all instances
    bool m_enableSerialUpdate{false};
    std::thread m_hotplugThread;
    std::thread m_deviceEnumerationThread;
    std::atomic<bool> m_enumerationThreadRunning{false};
    std::atomic<bool> m_hasPendingDevices{false};
    std::mutex m_pendingDevicesMutex;
    std::condition_variable m_pendingDevicesCV;

    std::set<std::string> m_activeDevices;  // Track USB paths of currently opened devices
    std::mutex m_activeDevicesMutex;
};
