// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "win_usb_transport.hpp"
#include "astra_log.hpp"
#include <initguid.h>
#include <devpkey.h>
#include <usbiodef.h>

WinUSBTransport::~WinUSBTransport()
{
    ASTRA_LOG;
    Shutdown();

    if (m_hCriticalSectionMutex) {
        CloseHandle(m_hCriticalSectionMutex);
        m_hCriticalSectionMutex = nullptr;
    }
}

int WinUSBTransport::Init(uint16_t vendorId, uint16_t productId, const std::string filterPorts, std::function<void(std::unique_ptr<USBDevice>)> deviceAddedCallback)
{
    ASTRA_LOG;

    m_vendorId = vendorId;
    m_productId = productId;

    m_filterPorts = ParseFilterPortString(filterPorts);

    int ret = libusb_init(&m_ctx);
    if (ret < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to initialize libusb: " << libusb_error_name(ret) << endLog;
        return ret;
    }

    if (m_usbDebug) {
        libusb_set_option(m_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG);
    }

    m_deviceAddedCallback = deviceAddedCallback;

    // Create a named mutex to serialize the critical boot section across all astra-boot instances
    // This prevents firmware hangs when multiple devices reset simultaneously after loading miniloader
    // The mutex provides automatic crash recovery via WAIT_ABANDONED
    m_hCriticalSectionMutex = CreateMutex(nullptr, FALSE, TEXT("Global\\AstraManagerCriticalSection"));
    if (!m_hCriticalSectionMutex) {
        DWORD error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS) {
            // Mutex already exists, which is fine - we'll use the existing one
            m_hCriticalSectionMutex = OpenMutex(SYNCHRONIZE, FALSE, TEXT("Global\\AstraManagerCriticalSection"));
        }
        if (!m_hCriticalSectionMutex) {
            log(ASTRA_LOG_LEVEL_WARNING) << "Failed to create critical section mutex: " << GetLastError() << endLog;
        }
    }

    m_running.store(true);

    // Start device enumeration worker thread
    m_enumerationThreadRunning.store(true);
    m_deviceEnumerationThread = std::thread(&WinUSBTransport::DeviceEnumerationWorker, this);

    m_hotplugThread = std::thread(&WinUSBTransport::RunHotplugHandler, this);

    StartDeviceMonitor();

    return ret;
}

void WinUSBTransport::Shutdown()
{
    ASTRA_LOG;

    std::lock_guard<std::mutex> lock(m_shutdownMutex);
    if (m_running.exchange(false)) {
        // Stop device enumeration worker thread
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
            DestroyWindow(m_hWnd);
            UnregisterClass(TEXT("AstraDeviceManager"), GetModuleHandle(nullptr));
            m_hWnd = nullptr;
        }

        if (m_deviceMonitorThread.joinable()) {
            m_deviceMonitorThread.join();
        }
    }
}

void WinUSBTransport::RunHotplugHandler()
{
    ASTRA_LOG;

    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WinUSBTransport::WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = TEXT("AstraDeviceManager");

    if (!RegisterClass(&wc)) {
        DWORD error = GetLastError();
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to register window class: " << error << endLog;
        return;
    }

    m_hWnd = CreateWindow(wc.lpszClassName, TEXT("AstraDeviceManager"), 0, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
    if (!m_hWnd) {
        DWORD error = GetLastError();
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to create window: " << error << endLog;
        return;
    }

    DEV_BROADCAST_DEVICEINTERFACE dbi = { 0 };
    dbi.dbcc_size = sizeof(dbi);
    dbi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    dbi.dbcc_reserved = 0;
    dbi.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;

    m_hDevNotify = RegisterDeviceNotification(m_hWnd, &dbi, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!m_hDevNotify) {
        DWORD error = GetLastError();
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to register device notification: " << error << endLog;
        return;
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

LRESULT CALLBACK WinUSBTransport::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    ASTRA_LOG;

    if (message == WM_DEVICECHANGE) {
        WinUSBTransport* handler = reinterpret_cast<WinUSBTransport*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

        if (wParam == DBT_DEVICEARRIVAL) {
            PDEV_BROADCAST_HDR pHdr = reinterpret_cast<PDEV_BROADCAST_HDR>(lParam);
            if (pHdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                PDEV_BROADCAST_DEVICEINTERFACE pDevInf = reinterpret_cast<PDEV_BROADCAST_DEVICEINTERFACE>(pHdr);
                log(ASTRA_LOG_LEVEL_DEBUG) << "Device Arrived: " << pDevInf->dbcc_name << endLog;
                if (handler) {
                    handler->OnDeviceArrived();
                }
            }
        }
    } else if (message == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreate->lpCreateParams));
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

void WinUSBTransport::OnDeviceArrived()
{
    ASTRA_LOG;

    // Defer device enumeration to avoid blocking the hotplug thread
    // and minimize lock contention with libusb event handling
    m_hasPendingDevices.store(true);
    m_pendingDevicesCV.notify_one();
}

void WinUSBTransport::DeviceEnumerationWorker()
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
        }

        // Process devices outside the lock
        if (m_hasPendingDevices.exchange(false)) {
            ProcessPendingDevices();
        }
    }
}

void WinUSBTransport::ProcessPendingDevices()
{
    ASTRA_LOG;

    bool retry = false;
    DWORD mutexAcquired = WAIT_FAILED;

    // Acquire mutex for entire enumeration to prevent race with critical section
    // This serializes enumeration across all instances but prevents:
    // - Enumeration during critical section (device resetting)
    // - Critical section starting during enumeration
    if (m_hCriticalSectionMutex) {
        mutexAcquired = WaitForSingleObject(m_hCriticalSectionMutex, 30000); // 30 second timeout
        if (mutexAcquired == WAIT_OBJECT_0) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Acquired mutex for enumeration" << endLog;
        } else if (mutexAcquired == WAIT_ABANDONED) {
            log(ASTRA_LOG_LEVEL_WARNING) << "Acquired abandoned mutex for enumeration (previous owner crashed)" << endLog;
        } else if (mutexAcquired == WAIT_TIMEOUT) {
            log(ASTRA_LOG_LEVEL_WARNING) << "Timeout waiting for enumeration mutex, skipping" << endLog;
            return;
        } else {
            log(ASTRA_LOG_LEVEL_WARNING) << "Failed to acquire enumeration mutex: " << mutexAcquired << ", skipping" << endLog;
            return;
        }
    }

    // Let devices settle
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (int retryCount = 0; retryCount < 3; ++retryCount) {
        libusb_device **device_list;
        ssize_t count = libusb_get_device_list(m_ctx, &device_list);
        if (count < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to get device list: " << libusb_error_name(count) << endLog;
            if (m_hCriticalSectionMutex && (mutexAcquired == WAIT_OBJECT_0 || mutexAcquired == WAIT_ABANDONED)) {
                ReleaseMutex(m_hCriticalSectionMutex);
            }
            return;
        }

        for (ssize_t i = 0; i < count; ++i) {
            libusb_device *device = device_list[i];
            libusb_device_descriptor desc;
            int ret = libusb_get_device_descriptor(device, &desc);
            if (ret < 0) {
                log(ASTRA_LOG_LEVEL_ERROR) << "Failed to get device descriptor" << endLog;
                continue;
            }

            std::string usbPath = ConstructUSBPath(device);
            if (!IsValidPort(device, usbPath)) {
                log(ASTRA_LOG_LEVEL_DEBUG) << "Device is not on a port we are monitoring" << endLog;
                continue;
            }

            if (desc.idVendor == m_vendorId && desc.idProduct == m_productId) {
                // Windows calls OnDeviceArrived() when any USB device arrives, not
                // just devices with a specific vid / pid. All USB devices are enumerated
                // in this loop, including devices already in use by astra-update.
                // On Windows try to open the device, if we get LIBUSB_ERROR_ACCESS then
                // we are probably already using the device.
                libusb_device_handle *handle;
                ret = libusb_open(device, &handle);
                if (ret == LIBUSB_ERROR_ACCESS) {
                    log(ASTRA_LOG_LEVEL_DEBUG) << "Device: " << usbPath << " open reported LIBUSB_ERROR_ACCESS" << endLog;
                    continue;
                } else if (ret == LIBUSB_ERROR_NO_DEVICE || ret == LIBUSB_ERROR_NOT_SUPPORTED) {
                    log(ASTRA_LOG_LEVEL_DEBUG) << "Device: " << usbPath << " no longer present" << endLog;
                    retry = true;
                    break;
                } else if (ret != 0) {
                    log(ASTRA_LOG_LEVEL_DEBUG) << "Device: " << usbPath << " open reported error: " << ret << endLog;
                    continue;
                }

                retry = false;
                std::unique_ptr<USBDevice> usbDevice = std::make_unique<USBDevice>(device, usbPath, m_ctx, handle);
                if (m_deviceAddedCallback) {
                    try {
                        m_deviceAddedCallback(std::move(usbDevice));
                    } catch (const std::bad_function_call& e) {
                        log(ASTRA_LOG_LEVEL_ERROR) << "Error: " << e.what() << endLog;
                    }
                } else {
                    log(ASTRA_LOG_LEVEL_ERROR) << "No device added callback" << endLog;
                }
            }
        }

        libusb_free_device_list(device_list, 1);

        if (retry) {
            retry = false;
            log(ASTRA_LOG_LEVEL_DEBUG) << "Retrying!" << endLog;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } else {
            break;
        }
    }

    // Release mutex after enumeration completes
    if (m_hCriticalSectionMutex && (mutexAcquired == WAIT_OBJECT_0 || mutexAcquired == WAIT_ABANDONED)) {
        ReleaseMutex(m_hCriticalSectionMutex);
        log(ASTRA_LOG_LEVEL_DEBUG) << "Released mutex after enumeration" << endLog;
    }
}

void WinUSBTransport::BlockDeviceEnumeration()
{
    ASTRA_LOG;

    if (m_hCriticalSectionMutex) {
        // Acquire the mutex to serialize critical boot section across all instances
        // This prevents device detection during critical boot sequences.
        DWORD waitResult = WaitForSingleObject(m_hCriticalSectionMutex, 30000); // 30 second timeout
        if (waitResult == WAIT_OBJECT_0) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Acquired critical section mutex" << endLog;
        } else if (waitResult == WAIT_ABANDONED) {
            // Previous owner crashed - we now own the mutex and can proceed
            log(ASTRA_LOG_LEVEL_WARNING) << "Acquired abandoned critical section mutex (previous owner crashed)" << endLog;
        } else if (waitResult == WAIT_TIMEOUT) {
            log(ASTRA_LOG_LEVEL_ERROR) << "Timeout waiting for critical section mutex" << endLog;
        } else {
            log(ASTRA_LOG_LEVEL_ERROR) << "Failed to acquire critical section mutex: " << waitResult << endLog;
        }
    }
}

void WinUSBTransport::UnblockDeviceEnumeration()
{
    ASTRA_LOG;

    if (m_hCriticalSectionMutex) {
        // Release the mutex to allow next device to enter critical section
        if (!ReleaseMutex(m_hCriticalSectionMutex)) {
            log(ASTRA_LOG_LEVEL_WARNING) << "Failed to release critical section mutex: " << GetLastError() << endLog;
        } else {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Released critical section mutex" << endLog;
        }
    }
}