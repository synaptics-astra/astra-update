// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include "flash_image.hpp"

class EmmcFlashImage : public FlashImage
{
public:
    EmmcFlashImage(std::string imagePath, std::string bootImage, std::string chipName,
            std::string boardName, AstraSecureBootVersion secureBootVersion, AstraMemoryLayout memoryLayout,
            bool resetWhenComplete, std::unique_ptr<std::vector<std::map<std::string, std::string>>> manifestMaps) : FlashImage(FLASH_IMAGE_TYPE_EMMC, imagePath,
            bootImage, chipName, boardName, secureBootVersion, memoryLayout, resetWhenComplete, std::move(manifestMaps))
    {}
    virtual ~EmmcFlashImage()
    {}

    int Load() override;

private:
    void ParseEmmcImageList();
};