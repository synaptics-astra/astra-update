// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "astra_device.hpp"
#include "astra_device_impl_internal.hpp"
#include "astra_log.hpp"

AstraDevice::AstraDevice(std::unique_ptr<USBDevice> device, const std::string &tempDir,
    bool bootOnly, const std::string &bootCommand, AstraDeviceSeries deviceSeries)
{
    ASTRA_LOG;

    if (deviceSeries == ASTRA_SERIES_SL26XX) {
        pImpl = CreateAstraDeviceSL26XXImpl(std::move(device), tempDir, bootOnly, bootCommand);
    } else {
        if (deviceSeries != ASTRA_SERIES_SL16XX) {
            log(ASTRA_LOG_LEVEL_WARNING) << "Unsupported device series selected, falling back to SL16XX implementation" << endLog;
        }
        pImpl = CreateAstraDeviceSL16XXImpl(std::move(device), tempDir, bootOnly, bootCommand);
    }
}

AstraDevice::~AstraDevice() = default;

void AstraDevice::SetStatusCallback(std::function<void(AstraDeviceManagerResponse)> statusCallback)
{
    pImpl->SetStatusCallback(statusCallback);
}

int AstraDevice::Boot(std::shared_ptr<AstraBootImage> bootImage, AstraDeviceBootStage bootStage)
{
    return pImpl->Boot(bootImage, bootStage);
}

int AstraDevice::Update(std::shared_ptr<FlashImage> flashImage)
{
    return pImpl->Update(flashImage);
}

int AstraDevice::WaitForCompletion()
{
    return pImpl->WaitForCompletion();
}

int AstraDevice::SendToConsole(const std::string &data)
{
    return pImpl->SendToConsole(data);
}

int AstraDevice::ReceiveFromConsole(std::string &data)
{
    return pImpl->ReceiveFromConsole(data);
}

std::string AstraDevice::GetDeviceName()
{
    return pImpl->GetDeviceName();
}

std::string AstraDevice::GetUSBPath()
{
    return pImpl->GetUSBPath();
}

AstraDeviceStatus AstraDevice::GetDeviceStatus()
{
    return pImpl->GetDeviceStatus();
}

void AstraDevice::Close()
{
    pImpl->Close();
}

const std::string AstraDevice::AstraDeviceStatusToString(AstraDeviceStatus status)
{
    static const std::string statusStrings[] = {
        "ASTRA_DEVICE_STATUS_ADDED",
        "ASTRA_DEVICE_STATUS_OPENED",
        "ASTRA_DEVICE_STATUS_CLOSED",
        "ASTRA_DEVICE_STATUS_BOOT_START",
        "ASTRA_DEVICE_STATUS_BOOT_PROGRESS",
        "ASTRA_DEVICE_STATUS_BOOT_COMPLETE",
        "ASTRA_DEVICE_STATUS_BOOT_FAIL",
        "ASTRA_DEVICE_STATUS_UPDATE_START",
        "ASTRA_DEVICE_STATUS_UPDATE_PROGRESS",
        "ASTRA_DEVICE_STATUS_UPDATE_COMPLETE",
        "ASTRA_DEVICE_STATUS_UPDATE_FAIL",
        "ASTRA_DEVICE_STATUS_IMAGE_SEND_START",
        "ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS",
        "ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE",
        "ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL",
    };

    return statusStrings[status];
}

const std::string AstraDevice::AstraDeviceSeriesToString(AstraDeviceSeries series)
{
    switch (series) {
        case ASTRA_SERIES_SL16XX:
            return "SL16XX";
        case ASTRA_SERIES_SL26XX:
            return "SL26XX";
        case ASTRA_SERIES_SR1XX:
            return "SR1XX";
        default:
            return "UNKNOWN";
    }
}

AstraDeviceBootStage AstraDevice::BootStageFromString(const std::string &stage)
{
    if (stage == "bootloader") return ASTRA_DEVICE_BOOT_STAGE_BOOTLOADER;
    if (stage == "linux")      return ASTRA_DEVICE_BOOT_STAGE_LINUX;
    if (stage == "m52bl")      return ASTRA_DEVICE_BOOT_STAGE_M52BL;
    if (stage == "sysmgr")     return ASTRA_DEVICE_BOOT_STAGE_SYSMGR;
    return ASTRA_DEVICE_BOOT_STAGE_AUTO;
}
