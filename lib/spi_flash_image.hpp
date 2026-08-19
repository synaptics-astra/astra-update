// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include "flash_image.hpp"

class SpiFlashImage : public FlashImage
{
public:
    SpiFlashImage(std::string imagePath, std::string bootImage, std::string chipName,
            std::string boardName, AstraSecureBootVersion secureBootVersion, AstraMemoryLayout memoryLayout, AstraMemoryDDRType memoryDDRType,
            AstraUbootVersion ubootVersion, bool resetWhenComplete, std::unique_ptr<std::vector<std::map<std::string, std::string>>> manifestMaps) : FlashImage(FLASH_IMAGE_TYPE_SPI, imagePath,
            bootImage, chipName, boardName, secureBootVersion, memoryLayout, memoryDDRType, ubootVersion, resetWhenComplete, std::move(manifestMaps))
    {}
    virtual ~SpiFlashImage()
    {}

    int Load() override;
    void OnUbootVersionChanged() override;

private:
    struct SpiImageConfig {
        SpiImageConfig(const std::string& chipName, AstraUbootVersion ubootVersion)
        {
            if (chipName.compare(0, 5, "sl261") == 0) {
                // SL261x defaults (U-Boot 2025.01)
                readAddress = "0x10000000";
                writeFirstCopyAddress = "0";
                writeSecondCopyAddress = "0x200000";
                writeLength = "$filesize";
                eraseFirstStartAddress = "0";
                eraseFirstLength = "0x200000";
                eraseSecondStartAddress = "0x200000";
                eraseSecondLength = "0x200000";
            } else if (ubootVersion == ASTRA_UBOOT_VERSION_2019_10) {
                // SL16x0 + U-Boot 2019.10 defaults
                readAddress = "0x10000000";
                writeFirstCopyAddress = "0xf0000000";
                writeSecondCopyAddress = "0xf0200000";
                writeLength = "0x200000";
                eraseFirstStartAddress = "0xf0000000";
                eraseFirstLength = "0xf01fffff";
                eraseSecondStartAddress = "0xf0200000";
                eraseSecondLength = "0xf03fffff";
            } else {
                // SL16x0 + U-Boot 2025.01 defaults
                readAddress = "0x10000000";
                writeFirstCopyAddress = "0";
                writeSecondCopyAddress = "0x200000";
                writeLength = "0x200000";
                eraseFirstStartAddress = "0";
                eraseFirstLength = "0x200000";
                eraseSecondStartAddress = "0x200000";
                eraseSecondLength = "0x200000";
            }
        }
        std::string imageFile;
        std::string readAddress;
        std::string writeFirstCopyAddress;
        std::string writeSecondCopyAddress;
        std::string writeLength;
        std::string eraseFirstStartAddress;
        std::string eraseFirstLength;
        std::string eraseSecondStartAddress;
        std::string eraseSecondLength;
    };

    // Per-image file name and raw config overrides from the manifest,
    // stored during Load() before the U-Boot version is known.
    struct SpiImageOverride {
        std::string imageFile;
        std::map<std::string, std::string> configOverrides;
    };
    std::vector<SpiImageOverride> m_spiImageOverrides;

    void ParseSpiFlashConfig(const std::map<std::string, std::string> &config, std::string imageFile);

    // Check every manifest value that reaches the U-Boot command line.
    // Sets m_loadError and returns false on the first unsafe value.
    bool ValidateOverrides();

    void BuildFlashCommand();
};