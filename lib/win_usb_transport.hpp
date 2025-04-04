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
    WinUSBTransport(bool usbDebug) : USBTransport(usbDebug) {};
    ~WinUSBTransport() override;
    int Init(uint16_t vendorId, uint16_t productId, std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback) override;
    void Shutdown() override;

private:
    void RunHotplugHandler();
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    void OnDeviceArrived();

    std::function<void(std::unique_ptr<USBDevice>)> m_deviceAddedCallback;
    HWND m_hWnd;
    HDEVNOTIFY m_hDevNotify;
    std::thread m_hotplugThread;
};