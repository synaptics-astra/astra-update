// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <windows.h>
#include <dbt.h>
#include <thread>
#include <memory>

#include "usb_transport.hpp"
#include "usb_device.hpp"

class WinUSBTransport : public USBTransport {
public:
    WinUSBTransport(bool usbDebug) : USBTransport(usbDebug), m_hWnd(nullptr), m_hDevNotify(nullptr), m_hCriticalSectionMutex(nullptr) {};
    ~WinUSBTransport() override;
    int Init(uint16_t vendorId, uint16_t productId, const std::string filterPorts, std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback) override;
    void Shutdown() override;

    void BlockDeviceEnumeration() override;
    void UnblockDeviceEnumeration() override;

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
    std::thread m_hotplugThread;
    std::thread m_deviceEnumerationThread;
    std::atomic<bool> m_enumerationThreadRunning{false};
    std::atomic<bool> m_hasPendingDevices{false};
    std::mutex m_pendingDevicesMutex;
    std::condition_variable m_pendingDevicesCV;
};