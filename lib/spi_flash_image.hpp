// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include "flash_image.hpp"

class SpiFlashImage : public FlashImage
{
public:
    SpiFlashImage(std::string imagePath, std::string bootImage, std::string chipName,
            std::string boardName, AstraSecureBootVersion secureBootVersion, AstraMemoryLayout memoryLayout,
            std::map<std::string, std::string> config) : FlashImage(FLASH_IMAGE_TYPE_SPI, imagePath,
            bootImage, chipName, boardName, secureBootVersion, memoryLayout, config)
    {}
    virtual ~SpiFlashImage()
    {}

    int Load() override;

private:
    std::string m_readAddress = "0x10000000";
    std::string m_writeFirstCopyAddress = "0xf0000000";
    std::string m_writeSecondCopyAddress = "0xf0200000";
    std::string m_writeLength = "0x200000";
    std::string m_eraseFirstStartAddress = "0xf0000000";
    std::string m_eraseFirstEndAddress = "0xf01fffff";
    std::string m_eraseSecondStartAddress = "0xf0200000";
    std::string m_eraseSecondEndAddress = "0xf03fffff";
};