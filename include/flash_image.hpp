// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <string>
#include <memory>
#include <vector>
#include <map>

#include "image.hpp"

enum FlashImageType {
    FLASH_IMAGE_TYPE_UNKNOWN,
    FLASH_IMAGE_TYPE_SPI,
    FLASH_IMAGE_TYPE_NAND,
    FLASH_IMAGE_TYPE_EMMC,
};

class FlashImage
{
public:
    FlashImage(FlashImageType flashImageType, std::string imagePath, std::string bootImageId, std::string chipName,
        std::string boardName, AstraSecureBootVersion secureBootVersion, AstraMemoryLayout memoryLayout, AstraMemoryDDRType memoryDDRType, bool resetWhenComplete,
        std::unique_ptr<std::vector<std::map<std::string, std::string>>> manifestMaps) : m_flashImageType{flashImageType}, m_imagePath{imagePath}, m_bootImageId{bootImageId},
        m_chipName{chipName}, m_boardName{boardName}, m_secureBootVersion{secureBootVersion}, m_memoryLayout{memoryLayout}, m_memoryDDRType{memoryDDRType}, m_resetWhenComplete{resetWhenComplete}, m_manifestMaps{std::move(manifestMaps)}
    {}
    virtual ~FlashImage()
    {}

    virtual int Load() = 0;

    std::string GetBootImageId() const { return m_bootImageId; }
    std::string GetChipName() const { return m_chipName; }
    std::string GetBoardName() const { return m_boardName; }
    std::string GetFlashCommand() const { return m_flashCommand; }
    const std::string &GetFinalImage() const { return m_finalImage; }
    AstraSecureBootVersion GetSecureBootVersion() const { return m_secureBootVersion; }
    AstraMemoryLayout GetMemoryLayout() const { return m_memoryLayout; }
    AstraMemoryDDRType GetMemoryDDRType() const { return m_memoryDDRType; }
    const std::vector<Image>& GetImages() const { return m_images; }
    FlashImageType GetFlashImageType() const { return m_flashImageType; }
    bool GetResetWhenComplete() const { return m_resetWhenComplete; }

    static std::shared_ptr<FlashImage> FlashImageFactory(std::string imagePath, std::map<std::string, std::string> &config, std::string manifest="");

protected:
    FlashImageType m_flashImageType;
    std::string m_bootImageId;
    std::string m_chipName;
    std::string m_boardName;
    AstraSecureBootVersion m_secureBootVersion;
    AstraMemoryLayout m_memoryLayout;
    AstraMemoryDDRType m_memoryDDRType;
    std::string m_imagePath;
    std::vector<Image> m_images;
    std::string m_flashCommand;
    std::string m_finalImage;
    std::unique_ptr<std::vector<std::map<std::string, std::string>>> m_manifestMaps;
    bool m_resetWhenComplete = true;
    const std::string m_resetCommand = "; sleep 1; reset"; // sleep before resetting to let console messages be sent to the host
};

static std::string AstraFlashImageTypeToString(FlashImageType type)
{
    switch (type) {
        case FLASH_IMAGE_TYPE_SPI:
            return "SPI";
        case FLASH_IMAGE_TYPE_NAND:
            return "NAND";
        case FLASH_IMAGE_TYPE_EMMC:
            return "eMMC";
        default:
            return "Unknown";
    }
}