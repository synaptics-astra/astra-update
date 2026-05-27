// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#pragma once

#include "flash_image.hpp"

class NandFlashImage : public FlashImage
{
public:
    NandFlashImage(std::string imagePath, std::string bootImage, std::string chipName,
            std::string boardName, AstraSecureBootVersion secureBootVersion, AstraMemoryLayout memoryLayout, AstraMemoryDDRType memoryDDRType,
            bool resetWhenComplete, std::unique_ptr<std::vector<std::map<std::string, std::string>>> manifestMaps) : FlashImage(FLASH_IMAGE_TYPE_NAND, imagePath,
            bootImage, chipName, boardName, secureBootVersion, memoryLayout, memoryDDRType, resetWhenComplete, std::move(manifestMaps))
    {}
    virtual ~NandFlashImage()
    {}

    int Load() override;

private:
    std::string m_imageFile;
    std::string m_nandReadAddress;
};