// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <memory>
#include <cstdint>
#include <functional>

#include "flash_image.hpp"

enum AstraDeviceStatus {
    ASTRA_DEVICE_STATUS_ADDED,
    ASTRA_DEVICE_STATUS_OPENED,
    ASTRA_DEVICE_STATUS_CLOSED,
    ASTRA_DEVICE_STATUS_BOOT_START,
    ASTRA_DEVICE_STATUS_BOOT_PROGRESS,
    ASTRA_DEVICE_STATUS_BOOT_COMPLETE,
    ASTRA_DEVICE_STATUS_BOOT_FAIL,
    ASTRA_DEVICE_STATUS_UPDATE_START,
    ASTRA_DEVICE_STATUS_UPDATE_PROGRESS,
    ASTRA_DEVICE_STATUS_UPDATE_COMPLETE,
    ASTRA_DEVICE_STATUS_UPDATE_FAIL,
    ASTRA_DEVICE_STATUS_IMAGE_SEND_START,
    ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS,
    ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE,
    ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL,
};

enum AstraDeviceSeries {
    ASTRA_SERIES_SL16XX,
    ASTRA_SERIES_SL26XX,
    ASTRA_SERIES_SR1XX,
};

enum AstraDeviceBootStage {
    ASTRA_DEVICE_BOOT_STAGE_AUTO,
    ASTRA_DEVICE_BOOT_STAGE_BOOTLOADER,
    ASTRA_DEVICE_BOOT_STAGE_LINUX,
    ASTRA_DEVICE_BOOT_STAGE_M52BL,   // SL26XX Only
    ASTRA_DEVICE_BOOT_STAGE_SYSMGR,  // SL26XX Only
};

class USBDevice;
class AstraBootImage;
class AstraDeviceManagerResponse;
class AstraDeviceImpl;

class AstraDevice
{
public:
    AstraDevice(std::unique_ptr<USBDevice> device, const std::string &tempDir, bool bootOnly, const std::string &bootCommand, AstraDeviceSeries deviceSeries = ASTRA_SERIES_SL16XX);
    ~AstraDevice();

    void SetStatusCallback(std::function<void(AstraDeviceManagerResponse)> statusCallback);

    int Boot(std::shared_ptr<AstraBootImage> bootImages, AstraDeviceBootStage bootStage = ASTRA_DEVICE_BOOT_STAGE_AUTO);
    int Update(std::shared_ptr<FlashImage> flashImage);
    int WaitForCompletion();

    int SendToConsole(const std::string &data);
    int ReceiveFromConsole(std::string &data);

    std::string GetDeviceName();
    std::string GetUSBPath();
    AstraDeviceStatus GetDeviceStatus();

    void Close();

    static const std::string AstraDeviceStatusToString(AstraDeviceStatus status);
    static const std::string AstraDeviceSeriesToString(AstraDeviceSeries series);
    static AstraDeviceBootStage BootStageFromString(const std::string &stage);

private:
    std::unique_ptr<AstraDeviceImpl> pImpl;
};

struct DeviceResponse
{
    std::string m_deviceName;
    AstraDeviceStatus m_status;
    double m_progress;
    std::string m_imageName;
    std::string m_message;
};
