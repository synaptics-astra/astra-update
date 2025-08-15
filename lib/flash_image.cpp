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

    std::unique_ptr<std::vector<std::map<std::string, std::string>>> manifestMaps = std::make_unique<std::vector<std::map<std::string, std::string>>>();
    std::map<std::string, std::string> configMap = config;
    configMap["type"] = "config";

    try {
        YAML::Node manifestNode = YAML::LoadFile(manifest);

        // If the image has a manifest file, but options were supplied on the command line,
        // then have the command line options take precedence.
         for (YAML::const_iterator it = manifestNode.begin(); it != manifestNode.end(); ++it) {
            std::string key = it->first.as<std::string>();
            const YAML::Node& value = it->second;

            if (configMap.find(key) == configMap.end()) {
                if (value.IsScalar()) {
                    configMap[key] = value.as<std::string>();
                } else {
                    if (key == "images") {
                        for (const auto& imageEntry : value) {
                            std::string imageName = imageEntry.first.as<std::string>();
                            YAML::Node imageProps = imageEntry.second;

                            if (imageProps.IsMap()) {
                                std::map<std::string, std::string> imageMap;
                                imageMap["type"] = "image";
                                imageMap["image_file"] = imageName;

                                for (const auto& prop : imageProps) {
                                    std::string propName = prop.first.as<std::string>();
                                    std::string propVal = prop.second.as<std::string>();
                                    imageMap[propName] = propVal;
                                }

                                manifestMaps->push_back(imageMap);
                            }
                        }
                    }
                }
            }
        }
        manifestMaps->push_back(configMap);
    } catch (const YAML::BadFile& e) {
        ;; // No manifest file, but we might have command line values
    } catch (const std::exception& e) {
        throw std::invalid_argument("Invalid Manifest");
    }

    std::string bootImage = configMap["boot_image"];
    std::string chipName = configMap["chip"];
    std::transform(chipName.begin(), chipName.end(), chipName.begin(), ::tolower);

    std::string boardName = configMap["board"];
    std::transform(boardName.begin(), boardName.end(), boardName.begin(), ::tolower);

    FlashImageType flashImageType = FLASH_IMAGE_TYPE_UNKNOWN;
    if (configMap.find("image_type") != configMap.end()) {
        flashImageType = StringToFlashImageType(configMap["image_type"]);
    }

    std::string secureBoot = configMap["secure_boot"];
    std::string memoryLayoutString = configMap["memory_layout"];

    std::string memoryDDRTypeString = configMap["ddr_type"];

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

    AstraMemoryDDRType memoryDDRType = ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED;
    std::transform(memoryDDRTypeString.begin(), memoryDDRTypeString.end(), memoryDDRTypeString.begin(), ::tolower);

    if (memoryDDRTypeString == "not_specified") {
        memoryDDRType = ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED;
    } else if (memoryDDRTypeString == "ddr3") {
        memoryDDRType = ASTRA_MEMORY_DDR_TYPE_DDR3;
    } else if (memoryDDRTypeString == "ddr4") {
        memoryDDRType = ASTRA_MEMORY_DDR_TYPE_DDR4;
    } else if (memoryDDRTypeString == "lpddr4") {
        memoryDDRType = ASTRA_MEMORY_DDR_TYPE_LPDDR4;
    } else if (memoryDDRTypeString == "lpddr4x") {
        memoryDDRType = ASTRA_MEMORY_DDR_TYPE_LPDDR4X;
    } else if (memoryDDRTypeString == "ddr4x16") {
        memoryDDRType = ASTRA_MEMORY_DDR_TYPE_DDR4X16;
    }

    if (flashImageType == FLASH_IMAGE_TYPE_UNKNOWN) {
        if (std::filesystem::exists(imagePath) && std::filesystem::is_directory(imagePath)
          && std::filesystem::exists(imagePath + "/emmc_part_list"))
        {
            // Image matches the structure of an eMMC image
            flashImageType = FLASH_IMAGE_TYPE_EMMC;
        }
    }

    bool resetWhenComplete = true;
    if (configMap.find("reset") != configMap.end()) {
        resetWhenComplete = configMap["reset"] == "enable";
    }

    switch (flashImageType) {
        case FLASH_IMAGE_TYPE_SPI:
            return std::make_shared<SpiFlashImage>(imagePath, bootImage, chipName, boardName, secureBootVersion,
                        memoryLayout, memoryDDRType, resetWhenComplete, std::move(manifestMaps));
        case FLASH_IMAGE_TYPE_NAND:
            throw std::invalid_argument("NAND FlashImage not supported");
        case FLASH_IMAGE_TYPE_EMMC:
            return std::make_shared<EmmcFlashImage>(imagePath, bootImage, chipName, boardName, secureBootVersion,
                memoryLayout, memoryDDRType, resetWhenComplete, std::move(manifestMaps));
        default:
            throw std::invalid_argument("Unknown FlashImageType");
    }
}
