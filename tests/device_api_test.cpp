// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <map>

#include "astra_device_manager.hpp"
#include "test_support.hpp"

namespace {

class ValueFlashImage : public FlashImage
{
public:
    ValueFlashImage()
        : FlashImage(FLASH_IMAGE_TYPE_SPI, "unused", "boot-id", "sl2619", "rdk",
            ASTRA_SECURE_BOOT_V3, ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_DDR4,
            ASTRA_UBOOT_VERSION_UNKNOWN, false, nullptr)
    {
        m_flashCommand = "command";
        m_finalImage = "final.bin";
        m_loadError = "load error";
        m_images.emplace_back("unused", ASTRA_IMAGE_TYPE_UPDATE_SPI);
    }
    int Load() override { return 0; }
    void OnUbootVersionChanged() override { ++versionChanges; }
    int versionChanges = 0;
};

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    const std::vector<std::pair<AstraDeviceStatus, std::string>> statuses = {
        {ASTRA_DEVICE_STATUS_ADDED, "ASTRA_DEVICE_STATUS_ADDED"},
        {ASTRA_DEVICE_STATUS_OPENED, "ASTRA_DEVICE_STATUS_OPENED"},
        {ASTRA_DEVICE_STATUS_CLOSED, "ASTRA_DEVICE_STATUS_CLOSED"},
        {ASTRA_DEVICE_STATUS_BOOT_START, "ASTRA_DEVICE_STATUS_BOOT_START"},
        {ASTRA_DEVICE_STATUS_BOOT_PROGRESS, "ASTRA_DEVICE_STATUS_BOOT_PROGRESS"},
        {ASTRA_DEVICE_STATUS_BOOT_COMPLETE, "ASTRA_DEVICE_STATUS_BOOT_COMPLETE"},
        {ASTRA_DEVICE_STATUS_BOOT_FAIL, "ASTRA_DEVICE_STATUS_BOOT_FAIL"},
        {ASTRA_DEVICE_STATUS_UPDATE_START, "ASTRA_DEVICE_STATUS_UPDATE_START"},
        {ASTRA_DEVICE_STATUS_UPDATE_PROGRESS, "ASTRA_DEVICE_STATUS_UPDATE_PROGRESS"},
        {ASTRA_DEVICE_STATUS_UPDATE_COMPLETE, "ASTRA_DEVICE_STATUS_UPDATE_COMPLETE"},
        {ASTRA_DEVICE_STATUS_UPDATE_FAIL, "ASTRA_DEVICE_STATUS_UPDATE_FAIL"},
        {ASTRA_DEVICE_STATUS_IMAGE_SEND_START, "ASTRA_DEVICE_STATUS_IMAGE_SEND_START"},
        {ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS, "ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS"},
        {ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE, "ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE"},
        {ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL, "ASTRA_DEVICE_STATUS_IMAGE_SEND_FAIL"}
    };
    for (const auto &status : statuses) {
        suite.Add("status-" + status.second, [=] {
            REQUIRE(AstraDevice::AstraDeviceStatusToString(status.first) == status.second);
        });
    }
    suite.Add("device-series-and-completion-commands", [] {
        const std::map<AstraDeviceSeries, std::string> series = {
            {ASTRA_SERIES_SL16XX, "SL16XX"}, {ASTRA_SERIES_SL26XX, "SL26XX"}, {ASTRA_SERIES_SR1XX, "SR1XX"}
        };
        for (const auto &entry : series) {
            REQUIRE(AstraDevice::AstraDeviceSeriesToString(entry.first) == entry.second);
            REQUIRE(AstraDevice::GetFlashCompletionCommand(entry.first) == "; sleep 1; reset");
        }
        REQUIRE(AstraDevice::AstraDeviceSeriesToString(static_cast<AstraDeviceSeries>(3)) == "UNKNOWN");
    });
    suite.Add("boot-stage-parsing-and-fallback", [] {
        const std::map<std::string, AstraDeviceBootStage> stages = {
            {"bootloader", ASTRA_DEVICE_BOOT_STAGE_BOOTLOADER}, {"linux", ASTRA_DEVICE_BOOT_STAGE_LINUX},
            {"m52bl", ASTRA_DEVICE_BOOT_STAGE_M52BL}, {"sysmgr", ASTRA_DEVICE_BOOT_STAGE_SYSMGR},
            {"auto", ASTRA_DEVICE_BOOT_STAGE_AUTO}, {"", ASTRA_DEVICE_BOOT_STAGE_AUTO},
            {"unknown", ASTRA_DEVICE_BOOT_STAGE_AUTO}, {"LINUX", ASTRA_DEVICE_BOOT_STAGE_AUTO}
        };
        for (const auto &entry : stages) {
            REQUIRE(AstraDevice::BootStageFromString(entry.first) == entry.second);
        }
    });
    suite.Add("image-enum-string-conversions", [] {
        REQUIRE(AstraSecureBootVersionToString(ASTRA_SECURE_BOOT_V2) == "gen2");
        REQUIRE(AstraSecureBootVersionToString(ASTRA_SECURE_BOOT_V3) == "genx");
        const std::map<AstraMemoryLayout, std::string> layouts = {
            {ASTRA_MEMORY_LAYOUT_1GB, "1GB"}, {ASTRA_MEMORY_LAYOUT_2GB, "2GB"},
            {ASTRA_MEMORY_LAYOUT_3GB, "3GB"}, {ASTRA_MEMORY_LAYOUT_4GB, "4GB"},
            {ASTRA_MEMORY_LAYOUT_512MB, "512MB"}
        };
        for (const auto &entry : layouts) {
            REQUIRE(AstraMemoryLayoutToString(entry.first) == entry.second);
        }
        REQUIRE(AstraMemoryLayoutToString(static_cast<AstraMemoryLayout>(5)) == "unknown");
        const std::map<AstraMemoryDDRType, std::string> ddr = {
            {ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, "not_specified"}, {ASTRA_MEMORY_DDR_TYPE_DDR3, "DDR3"},
            {ASTRA_MEMORY_DDR_TYPE_DDR4, "DDR4"}, {ASTRA_MEMORY_DDR_TYPE_LPDDR4, "LPDDR4"},
            {ASTRA_MEMORY_DDR_TYPE_LPDDR4X, "LPDDR4X"}, {ASTRA_MEMORY_DDR_TYPE_DDR4X16, "DDR4X16"}
        };
        for (const auto &entry : ddr) {
            REQUIRE(AstraMemoryDDRTypeToString(entry.first) == entry.second);
        }
        REQUIRE(AstraMemoryDDRTypeToString(static_cast<AstraMemoryDDRType>(6)) == "not_specified");
        REQUIRE(AstraTransportToString(ASTRA_TRANSPORT_USB) == "USB");
        REQUIRE(AstraTransportToString(ASTRA_TRANSPORT_USB_CDC) == "USB_CDC");
        REQUIRE(AstraFlashImageTypeToString(FLASH_IMAGE_TYPE_SPI) == "SPI");
        REQUIRE(AstraFlashImageTypeToString(FLASH_IMAGE_TYPE_NAND) == "NAND");
        REQUIRE(AstraFlashImageTypeToString(FLASH_IMAGE_TYPE_EMMC) == "eMMC");
        REQUIRE(AstraFlashImageTypeToString(FLASH_IMAGE_TYPE_UNKNOWN) == "Unknown");
    });
    for (const auto status : {ASTRA_DEVICE_MANAGER_STATUS_START, ASTRA_DEVICE_MANAGER_STATUS_INFO,
        ASTRA_DEVICE_MANAGER_STATUS_FAILURE, ASTRA_DEVICE_MANAGER_STATUS_SHUTDOWN}) {
        suite.Add("manager-response-" + std::to_string(status), [=] {
            AstraDeviceManagerResponse response(ManagerResponse{status, "message"});
            REQUIRE(response.IsDeviceManagerResponse() && !response.IsDeviceResponse());
            REQUIRE(response.GetDeviceManagerResponse().m_managerStatus == status);
            REQUIRE(response.GetDeviceManagerResponse().m_managerMessage == "message");
            REQUIRE_THROWS(std::bad_variant_access, response.GetDeviceResponse());
        });
    }
    for (const auto &status : statuses) {
        suite.Add("device-response-" + status.second, [=] {
            AstraDeviceManagerResponse response(DeviceResponse{"device1", status.first, 42.5, "image.bin", "message"});
            REQUIRE(response.IsDeviceResponse() && !response.IsDeviceManagerResponse());
            const auto &device = response.GetDeviceResponse();
            REQUIRE(device.m_deviceName == "device1" && device.m_status == status.first);
            REQUIRE(device.m_progress == 42.5 && device.m_imageName == "image.bin");
            REQUIRE(device.m_message == "message");
            REQUIRE_THROWS(std::bad_variant_access, response.GetDeviceManagerResponse());
            const auto copied = response;
            REQUIRE(copied.GetDeviceResponse().m_imageName == "image.bin");
        });
    }
    suite.Add("flash-image-value-getters-and-version-hook", [] {
        ValueFlashImage image;
        REQUIRE(image.GetBootImageId() == "boot-id");
        REQUIRE(image.GetChipName() == "sl2619" && image.GetBoardName() == "rdk");
        REQUIRE(image.GetFlashCommand() == "command" && image.GetFinalImage() == "final.bin");
        REQUIRE(image.GetLoadError() == "load error");
        REQUIRE(image.GetSecureBootVersion() == ASTRA_SECURE_BOOT_V3);
        REQUIRE(image.GetMemoryLayout() == ASTRA_MEMORY_LAYOUT_2GB);
        REQUIRE(image.GetMemoryDDRType() == ASTRA_MEMORY_DDR_TYPE_DDR4);
        REQUIRE(image.GetUbootVersion() == ASTRA_UBOOT_VERSION_UNKNOWN);
        REQUIRE(image.GetFlashImageType() == FLASH_IMAGE_TYPE_SPI);
        REQUIRE(!image.GetResetWhenComplete());
        REQUIRE(image.GetImages().size() == 1);
        image.SetUbootVersion(ASTRA_UBOOT_VERSION_2019_10);
        REQUIRE(image.GetUbootVersion() == ASTRA_UBOOT_VERSION_2019_10 && image.versionChanges == 1);
        image.SetUbootVersion(ASTRA_UBOOT_VERSION_2025_01);
        REQUIRE(image.GetUbootVersion() == ASTRA_UBOOT_VERSION_2025_01 && image.versionChanges == 2);
    });
    suite.Add("manager-version-is-public-version", [] {
        REQUIRE(AstraDeviceManager::GetVersion() == ASTRA_DEVICE_MANAGER_VERSION);
    });
    return suite.Run(argc, argv);
}
