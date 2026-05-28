// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "win_usb_cdc_transport.hpp"

#include "astra_log.hpp"
#include "win_usb_cdc_device.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <devguid.h>
#include <initguid.h>
#include <setupapi.h>
#include <sstream>
#include <usbiodef.h>

WinUSBCDCTransport::~WinUSBCDCTransport()
{
    ASTRA_LOG;
    Shutdown();
}

int WinUSBCDCTransport::Init(std::vector<USBVendorProductId> vendorProductIds, const std::string filterPorts,
    std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback)
{
    ASTRA_LOG;

    m_supportedDevices = vendorProductIds;

    m_filterPorts = ParseFilterPortString(filterPorts);
    for (auto &port : m_filterPorts) {
        port = NormalizePortPath(port);
    }

    m_deviceAddedCallback = deviceAddedCallback;

    m_running.store(true);

    m_enumerationThreadRunning.store(true);
    m_deviceEnumerationThread = std::thread(&WinUSBCDCTransport::DeviceEnumerationWorker, this);

    m_hotplugThread = std::thread(&WinUSBCDCTransport::RunHotplugHandler, this);

    // Trigger initial pass so already attached devices are discovered.
    m_hasPendingDevices.store(true);
    m_pendingDevicesCV.notify_one();

    return 0;
}

void WinUSBCDCTransport::Shutdown()
{
    ASTRA_LOG;

    std::lock_guard<std::mutex> lock(m_shutdownMutex);
    if (m_running.exchange(false)) {
        if (m_enumerationThreadRunning.exchange(false)) {
            m_pendingDevicesCV.notify_all();
            if (m_deviceEnumerationThread.joinable()) {
                m_deviceEnumerationThread.join();
            }
        }

        if (m_hWnd) {
            PostMessage(m_hWnd, WM_QUIT, 0, 0);
            if (m_hotplugThread.joinable()) {
                m_hotplugThread.join();
            }

            if (m_hDevNotify) {
                UnregisterDeviceNotification(m_hDevNotify);
                m_hDevNotify = nullptr;
            }

            DestroyWindow(m_hWnd);
            UnregisterClassA("AstraCDCDeviceManager", GetModuleHandle(nullptr));
            m_hWnd = nullptr;
        } else if (m_hotplugThread.joinable()) {
            m_hotplugThread.join();
        }

        std::lock_guard<std::mutex> activeLock(m_activeDevicesMutex);
        m_activeDevices.clear();
    }
}

void WinUSBCDCTransport::RunHotplugHandler()
{
    ASTRA_LOG;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WinUSBCDCTransport::WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "AstraCDCDeviceManager";

    if (!RegisterClassA(&wc)) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to register CDC window class: " << error << endLog;
            return;
        }
    }

    m_hWnd = CreateWindowA(wc.lpszClassName, "AstraCDCDeviceManager", 0,
        0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
    if (!m_hWnd) {
        const DWORD error = GetLastError();
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to create CDC window: " << error << endLog;
        return;
    }

    DEV_BROADCAST_DEVICEINTERFACE_A dbi = {};
    dbi.dbcc_size = sizeof(dbi);
    dbi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    dbi.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;

    m_hDevNotify = RegisterDeviceNotificationA(m_hWnd, &dbi, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!m_hDevNotify) {
        log(ASTRA_LOG_LEVEL_WARNING) << "Failed to register CDC device notifications: " << GetLastError() << endLog;
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

LRESULT CALLBACK WinUSBCDCTransport::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    ASTRA_LOG;

    if (message == WM_CREATE) {
        auto *create = reinterpret_cast<CREATESTRUCT *>(lParam);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    if (message == WM_DEVICECHANGE) {
        auto *handler = reinterpret_cast<WinUSBCDCTransport *>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
        if (handler && (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE)) {
            handler->OnDeviceChange();
        }
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

void WinUSBCDCTransport::OnDeviceChange()
{
    ASTRA_LOG;

    m_hasPendingDevices.store(true);
    m_pendingDevicesCV.notify_one();
}

void WinUSBCDCTransport::DeviceEnumerationWorker()
{
    ASTRA_LOG;

    while (m_enumerationThreadRunning.load()) {
        {
            std::unique_lock<std::mutex> lock(m_pendingDevicesMutex);
            m_pendingDevicesCV.wait(lock, [this] {
                return m_hasPendingDevices.load() || !m_enumerationThreadRunning.load();
            });

            if (!m_enumerationThreadRunning.load()) {
                break;
            }

            m_hasPendingDevices.store(false);
        }

        ProcessPendingDevices();
    }
}

void WinUSBCDCTransport::ProcessPendingDevices()
{
    ASTRA_LOG;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto ports = EnumerateMatchingPorts();

    // Build the set of currently-present normalized port names so we can purge
    // stale entries from m_activeDevices. This allows genuine re-plugs to be
    // detected after a device physically disconnects.
    std::set<std::string> presentPorts;
    for (const auto &enumeratedPort : ports) {
        presentPorts.insert(NormalizePortPath(enumeratedPort.m_port));
    }
    {
        std::lock_guard<std::mutex> lock(m_activeDevicesMutex);
        for (auto it = m_activeDevices.begin(); it != m_activeDevices.end(); ) {
            if (presentPorts.find(*it) == presentPorts.end()) {
                it = m_activeDevices.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const auto &enumeratedPort : ports) {
        const std::string port = NormalizePortPath(enumeratedPort.m_port);
        if (!IsValidPort(port)) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_activeDevicesMutex);
            if (m_activeDevices.find(port) != m_activeDevices.end()) {
                continue;
            }
            m_activeDevices.insert(port);
        }

        std::unique_ptr<USBDevice> usbDevice = std::make_unique<WinUSBCDCDevice>(port,
            enumeratedPort.m_vendorId, enumeratedPort.m_productId, enumeratedPort.m_numInterfaces);
        if (m_deviceAddedCallback) {
            try {
                m_deviceAddedCallback(std::move(usbDevice));
            } catch (...) {
                RemoveActiveDevice(port);
            }
        } else {
            RemoveActiveDevice(port);
        }
    }
}

std::vector<WinUSBCDCTransport::EnumeratedPort> WinUSBCDCTransport::EnumerateMatchingPorts() const
{
    ASTRA_LOG;

    std::vector<EnumeratedPort> ports;

    HDEVINFO deviceInfoSet = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return ports;
    }

    SP_DEVINFO_DATA deviceInfoData = {};
    deviceInfoData.cbSize = sizeof(deviceInfoData);

    for (DWORD index = 0; SetupDiEnumDeviceInfo(deviceInfoSet, index, &deviceInfoData); ++index) {
        char friendlyName[512] = {};
        DWORD regType = 0;
        DWORD requiredSize = 0;

        if (!SetupDiGetDeviceRegistryPropertyA(deviceInfoSet, &deviceInfoData, SPDRP_FRIENDLYNAME,
                &regType, reinterpret_cast<PBYTE>(friendlyName), sizeof(friendlyName), &requiredSize)) {
            continue;
        }

        const std::string port = ExtractComPortFromFriendlyName(friendlyName);
        if (port.empty()) {
            continue;
        }

        uint16_t detectedVid = 0;
        uint16_t detectedPid = 0;
        uint8_t detectedNumInterfaces = 0;
        char hardwareIds[1024] = {};
        if (SetupDiGetDeviceRegistryPropertyA(deviceInfoSet, &deviceInfoData, SPDRP_HARDWAREID,
                &regType, reinterpret_cast<PBYTE>(hardwareIds), sizeof(hardwareIds), &requiredSize)) {
            const std::string hwId(hardwareIds);
            ExtractVidPid(hwId, detectedVid, detectedPid);
            std::string upper = hwId;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            detectedNumInterfaces = (upper.find("MI_") != std::string::npos) ? uint8_t{2} : uint8_t{1};
        }

        bool vidPidMatches = m_supportedDevices.empty();
        if (!vidPidMatches && (detectedVid != 0 || detectedPid != 0)) {
            for (const auto& [vid, pid] : m_supportedDevices) {
                if (detectedVid == vid && detectedPid == pid) {
                    vidPidMatches = true;
                    break;
                }
            }
        }

        if (vidPidMatches) {
            ports.push_back({port, detectedVid, detectedPid, detectedNumInterfaces});
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return ports;
}

std::string WinUSBCDCTransport::ExtractComPortFromFriendlyName(const std::string& friendlyName)
{
    const std::size_t openParen = friendlyName.rfind('(');
    const std::size_t closeParen = friendlyName.rfind(')');
    if (openParen == std::string::npos || closeParen == std::string::npos || closeParen <= openParen + 1) {
        return std::string();
    }

    const std::string candidate = friendlyName.substr(openParen + 1, closeParen - openParen - 1);
    return candidate;
}

bool WinUSBCDCTransport::ExtractVidPid(const std::string& hardwareId, uint16_t &vendorId, uint16_t &productId)
{
    std::string normalized = hardwareId;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    const std::size_t vidPos = normalized.find("VID_");
    const std::size_t pidPos = normalized.find("PID_");
    if (vidPos == std::string::npos || pidPos == std::string::npos) {
        return false;
    }

    if (vidPos + 8 > normalized.size() || pidPos + 8 > normalized.size()) {
        return false;
    }

    try {
        vendorId = static_cast<uint16_t>(std::stoul(normalized.substr(vidPos + 4, 4), nullptr, 16));
        productId = static_cast<uint16_t>(std::stoul(normalized.substr(pidPos + 4, 4), nullptr, 16));
        return true;
    } catch (...) {
        return false;
    }
}

std::string WinUSBCDCTransport::NormalizePortPath(const std::string& portPath) const
{
    std::string normalized = portPath;

    static const std::string kComPrefix = "\\\\.\\";
    if (normalized.rfind(kComPrefix, 0) == 0) {
        normalized = normalized.substr(kComPrefix.size());
    }

    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    return normalized;
}

void WinUSBCDCTransport::RemoveActiveDevice(const std::string& usbPath)
{
    ASTRA_LOG;

    std::lock_guard<std::mutex> lock(m_activeDevicesMutex);
    m_activeDevices.erase(NormalizePortPath(usbPath));
}
