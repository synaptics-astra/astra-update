// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <stdexcept>
#include <iostream>
#include <yaml-cpp/yaml.h>
#include "flash_image.hpp"
#include "astra_log.hpp"

#include "emmc_flash_image.hpp"
#include "spi_flash_image.hpp"

const std::string FlashImageTypeToString(FlashImageType type)
{
    std::string str = "unknown";

    switch (type) {
        case FLASH_IMAGE_TYPE_SPI:
            str = "spi";
            break;
        case FLASH_IMAGE_TYPE_NAND:
            str = "nand";
            break;
        case FLASH_IMAGE_TYPE_EMMC:
            str = "emmc";
            break;
        case FLASH_IMAGE_TYPE_UNKNOWN:
            str = "unknown";
            break;
        default:
            break;
    }

    return str;
}

FlashImageType StringToFlashImageType(const std::string& str)
{
    if (str == "spi") {
        return FLASH_IMAGE_TYPE_SPI;
    } else if (str == "nand") {
        return FLASH_IMAGE_TYPE_NAND;
    } else if (str == "emmc") {
        return FLASH_IMAGE_TYPE_EMMC;
    } else {
        throw std::invalid_argument("Unknown FlashImageType string");
    }
}

std::shared_ptr<FlashImage> FlashImage::FlashImageFactory(std::string imagePath, std::map<std::string, std::string> &config, std::string manifest)
{
    if (manifest == "") {
        manifest = imagePath + "/manifest.yaml";
    }

    if (!std::filesystem::exists(imagePath)) {
        if (imagePath == "eMMCimg") {
            // If no image directory was specified and the default eMMCing does not exist
            // then try the SYNAIMG directory. Which is the default directory name created by the
            // Yocto build system.
            imagePath = "SYNAIMG";
        } else {
            throw std::invalid_argument("" + imagePath + " not found");
        }
    }

    try {
        YAML::Node manifestNode = YAML::LoadFile(manifest);

        // If the image has a manifest file, but options were supplied on the command line,
        // then have the command line options take precedence.
        for (YAML::const_iterator it = manifestNode.begin(); it != manifestNode.end(); ++it) {
            if (config.find(it->first.as<std::string>()) == config.end()) {
                config[it->first.as<std::string>()] = it->second.as<std::string>();
            }
        }
    }
    catch (const YAML::BadFile& e) {
        ;; // No manifest file, but we might have command line values
    } catch (const std::exception& e) {
        throw std::invalid_argument("Invalid Manifest");
    }

    std::string bootImage = config["boot_image"];
    std::string chipName = config["chip"];
    std::transform(chipName.begin(), chipName.end(), chipName.begin(), ::tolower);

    std::string boardName = config["board"];
    std::transform(boardName.begin(), boardName.end(), boardName.begin(), ::tolower);

    FlashImageType flashImageType = FLASH_IMAGE_TYPE_UNKNOWN;
    if (config.find("image_type") != config.end()) {
        flashImageType = StringToFlashImageType(config["image_type"]);
    }

    std::string secureBoot = config["secure_boot"];
    std::string memoryLayoutString = config["memory_layout"];

    std::transform(secureBoot.begin(), secureBoot.end(), secureBoot.begin(), ::tolower);
    AstraSecureBootVersion secureBootVersion = secureBoot == "gen2" ? ASTRA_SECURE_BOOT_V2 : ASTRA_SECURE_BOOT_V3;

    AstraMemoryLayout memoryLayout = ASTRA_MEMORY_LAYOUT_2GB;
    if (memoryLayoutString.empty()) {
        // Set default memory layouts based on the chip
        if (chipName == "sl1680") {
            memoryLayout = ASTRA_MEMORY_LAYOUT_4GB;
        }
    } else {
        std::transform(memoryLayoutString.begin(), memoryLayoutString.end(), memoryLayoutString.begin(), ::tolower);

        if (memoryLayoutString == "1gb") {
            memoryLayout = ASTRA_MEMORY_LAYOUT_1GB;
        } else if (memoryLayoutString == "2gb") {
            memoryLayout = ASTRA_MEMORY_LAYOUT_2GB;
        } else if (memoryLayoutString == "3gb") {
            memoryLayout = ASTRA_MEMORY_LAYOUT_3GB;
        } else if (memoryLayoutString == "4gb") {
            memoryLayout = ASTRA_MEMORY_LAYOUT_4GB;
        } else {
            throw std::invalid_argument("Invalid Memory Layout");
        }
    }

    if (flashImageType == FLASH_IMAGE_TYPE_UNKNOWN) {
        if (std::filesystem::exists(imagePath) && std::filesystem::is_directory(imagePath)
          && std::filesystem::exists(imagePath + "/emmc_part_list"))
        {
            // Image matches the structure of an eMMC image
            flashImageType = FLASH_IMAGE_TYPE_EMMC;
        }
    }

    switch (flashImageType) {
        case FLASH_IMAGE_TYPE_SPI:
            return std::make_shared<SpiFlashImage>(imagePath, bootImage, chipName, boardName, secureBootVersion, memoryLayout, config);
        case FLASH_IMAGE_TYPE_NAND:
            throw std::invalid_argument("NAND FlashImage not supported");
        case FLASH_IMAGE_TYPE_EMMC:
            return std::make_shared<EmmcFlashImage>(imagePath, bootImage, chipName, boardName, secureBootVersion, memoryLayout, config);
        default:
            throw std::invalid_argument("Unknown FlashImageType");
    }
}