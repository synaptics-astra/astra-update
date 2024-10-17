#pragma once

#include "flash_image.hpp"

class EmmcFlashImage : public FlashImage
{
public:
    EmmcFlashImage(std::string imagePath, std::string bootImage, std::string chipName,
            std::string boardName, AstraSecureBootVersion secureBootVersion, AstraMemoryLayout memoryLayout,
            std::map<std::string, std::string> config) : FlashImage(FLASH_IMAGE_TYPE_EMMC, imagePath,
            bootImage, chipName, boardName, secureBootVersion, memoryLayout, config)
    {}
    virtual ~EmmcFlashImage()
    {}

    int Load() override;

private:
    void ParseEmmcImageList();
};