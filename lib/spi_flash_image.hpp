// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include "flash_image.hpp"

class SpiFlashImage : public FlashImage
{
public:
    SpiFlashImage(std::string imagePath, std::string bootImage, std::string chipName,
            std::string boardName, AstraSecureBootVersion secureBootVersion, AstraMemoryLayout memoryLayout, AstraMemoryDDRType memoryDDRType,
            bool resetWhenComplete, std::unique_ptr<std::vector<std::map<std::string, std::string>>> manifestMaps) : FlashImage(FLASH_IMAGE_TYPE_SPI, imagePath,
            bootImage, chipName, boardName, secureBootVersion, memoryLayout, memoryDDRType, resetWhenComplete, std::move(manifestMaps))
    {}
    virtual ~SpiFlashImage()
    {}

    int Load() override;

private:
    struct SpiImageConfig {
        SpiImageConfig(const std::string chipName)
        {
            if (chipName.compare(0, 5, "sl261") == 0) {
                // Overwrite default config values for SL2610 series.
                readAddress = "0x10000000";
                writeFirstCopyAddress = "0";
                writeSecondCopyAddress = "0x200000";
                writeLength = "$filesize";
                eraseFirstStartAddress = "0";
                eraseFirstLength = "0x200000";
                eraseSecondStartAddress = "0x200000";
                eraseSecondLength = "0x200000";
            }
        }
        std::string imageFile;
        std::string readAddress = "0x10000000";
        std::string writeFirstCopyAddress = "0xf0000000";
        std::string writeSecondCopyAddress = "0xf0200000";
        std::string writeLength = "0x200000";
        std::string eraseFirstStartAddress = "0xf0000000";
        std::string eraseFirstLength = "0xf01fffff";
        std::string eraseSecondStartAddress = "0xf0200000";
        std::string eraseSecondLength = "0xf03fffff";
    };
    std::vector<SpiImageConfig> m_spiImageConfigs;

    void ParseSpiFlashConfig(const std::map<std::string, std::string> &config, std::string imageFile);
};