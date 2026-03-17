// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "astra_device.hpp"
#include "astra_device_manager.hpp"
#include "astra_log.hpp"
#include "usb_device.hpp"

class AstraDeviceImpl {
public:
    AstraDeviceImpl(std::unique_ptr<USBDevice> device, const std::string &tempDir,
        bool bootOnly, const std::string &bootCommand)
        : m_usbDevice{std::move(device)}, m_tempDir{tempDir}, m_bootOnly{bootOnly}, m_bootCommand{bootCommand}
    {
        ASTRA_LOG;
    }

    virtual ~AstraDeviceImpl()
    {
        ASTRA_LOG;
        Close();
    }

    virtual void SetStatusCallback(std::function<void(AstraDeviceManagerResponse)> statusCallback)
    {
        ASTRA_LOG;
        m_statusCallback = statusCallback;
    }

    virtual int Boot(std::shared_ptr<AstraBootImage> bootImage, AstraDeviceBootStage bootStage) = 0;
    virtual int Update(std::shared_ptr<FlashImage> flashImage) = 0;
    virtual int WaitForCompletion() = 0;
    virtual int SendToConsole(const std::string &data) = 0;
    virtual int ReceiveFromConsole(std::string &data) = 0;

    virtual std::string GetDeviceName()
    {
        return m_deviceName;
    }

    virtual std::string GetUSBPath()
    {
        if (m_usbDevice == nullptr) {
            return "";
        }
        return m_usbDevice->GetUSBPath();
    }

    virtual AstraDeviceStatus GetDeviceStatus()
    {
        return m_status;
    }

    virtual void Close()
    {
        ASTRA_LOG;

        std::lock_guard<std::mutex> lock(m_closeMutex);
        if (m_shutdown.exchange(true)) {
            return;
        }

        if (m_usbDevice != nullptr) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Closing USB device" << endLog;
            m_usbDevice->Close();
        }

        m_status = ASTRA_DEVICE_STATUS_CLOSED;
    }

protected:
    void ReportStatus(AstraDeviceStatus status, double progress, const std::string &imageName,
        const std::string &message = "")
    {
        ASTRA_LOG;

        log(ASTRA_LOG_LEVEL_INFO) << "Device status: " << AstraDevice::AstraDeviceStatusToString(status)
            << " Progress: " << progress << " Image: " << imageName << " Message: " << message << endLog;

        if (m_statusCallback) {
            m_statusCallback({DeviceResponse{m_deviceName, status, progress, imageName, message}});
        }
    }

    std::unique_ptr<USBDevice> m_usbDevice;
    AstraDeviceStatus m_status = ASTRA_DEVICE_STATUS_ADDED;
    std::function<void(AstraDeviceManagerResponse)> m_statusCallback;
    std::string m_deviceName;
    std::string m_tempDir;
    bool m_bootOnly = false;
    std::string m_bootCommand;
    AstraDeviceBootStage m_bootStage = ASTRA_DEVICE_BOOT_STAGE_AUTO;

private:
    std::atomic<bool> m_shutdown{false};
    std::mutex m_closeMutex;
};

std::unique_ptr<AstraDeviceImpl> CreateAstraDeviceSL16XXImpl(std::unique_ptr<USBDevice> device,
    const std::string &tempDir, bool bootOnly, const std::string &bootCommand);

std::unique_ptr<AstraDeviceImpl> CreateAstraDeviceSL26XXImpl(std::unique_ptr<USBDevice> device,
    const std::string &tempDir, bool bootOnly, const std::string &bootCommand);
