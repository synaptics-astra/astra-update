// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <stdexcept>
#include <iostream>
#include <filesystem>
#include <yaml-cpp/yaml.h>
#include <cctype>
#include "flash_image.hpp"
#include "astra_log.hpp"

#include "emmc_flash_image.hpp"
#include "spi_flash_image.hpp"
#include "nand_flash_image.hpp"

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
    ASTRA_LOG;

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
        // No manifest file, but we might have command line values
        log(ASTRA_LOG_LEVEL_DEBUG) << "No manifest file found at: " << manifest << endLog;
    } catch (const YAML::Exception& e) {
        // Invalid YAML syntax
        log(ASTRA_LOG_LEVEL_WARNING) << "Invalid manifest file: " << e.what() << endLog;
    } catch (const std::exception& e) {
        log(ASTRA_LOG_LEVEL_WARNING) << "Error parsing manifest: " << e.what() << endLog;
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

    // Set board-specific DDR type defaults when not explicitly configured
    if (memoryDDRType == ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED) {
        if (boardName == "coralboard") {
            memoryDDRType = ASTRA_MEMORY_DDR_TYPE_DDR4X16;
        }
    }

    if (flashImageType == FLASH_IMAGE_TYPE_UNKNOWN) {
        if (std::filesystem::exists(imagePath) && std::filesystem::is_directory(imagePath)) {
            // Check for NAND image by looking for a Yocto TAG file containing "nand"
            for (const auto& entry : std::filesystem::directory_iterator(imagePath)) {
                std::string filename = entry.path().filename().string();
                if (filename.find("TAG--") != std::string::npos &&
                    filename.find("astra") != std::string::npos &&
                    filename.find("nand") != std::string::npos)
                {
                    flashImageType = FLASH_IMAGE_TYPE_NAND;
                    break;
                }
            }

            // Check for eMMC image by looking for the partition list file
            if (flashImageType == FLASH_IMAGE_TYPE_UNKNOWN &&
                std::filesystem::exists(imagePath + "/emmc_part_list"))
            {
                flashImageType = FLASH_IMAGE_TYPE_EMMC;
            }
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
            return std::make_shared<NandFlashImage>(imagePath, bootImage, chipName, boardName, secureBootVersion,
                        memoryLayout, memoryDDRType, resetWhenComplete, std::move(manifestMaps));
        case FLASH_IMAGE_TYPE_EMMC:
            return std::make_shared<EmmcFlashImage>(imagePath, bootImage, chipName, boardName, secureBootVersion,
                memoryLayout, memoryDDRType, resetWhenComplete, std::move(manifestMaps));
        default:
            throw std::invalid_argument("Unknown FlashImageType");
    }
}

ChipDetectionResult DetectChipFromTagFile(const std::string& imagePath, const std::string& currentChipName)
{
    ASTRA_LOG;
    ChipDetectionResult result;

    if (!std::filesystem::exists(imagePath) || !std::filesystem::is_directory(imagePath)) {
        return result;
    }

    for (const auto& entry : std::filesystem::directory_iterator(imagePath)) {
        std::string filename = entry.path().filename().string();
        if ((filename.find("TAG--") != std::string::npos) && (filename.find("astra") != std::string::npos)) {
            // Yocto builds create a TAG file in the image directory. The name of the file
            // contains the chip name and image name. We use this to determine the chip name
            // and secure boot version if not provided in the config.

            std::size_t pos = filename.find("sl");
            if (pos != std::string::npos && pos + 6 <= filename.size()) {
                std::string potentialChipName = filename.substr(pos, 6);
                if (potentialChipName.size() == 6 && std::isdigit(potentialChipName[2]) && std::isdigit(potentialChipName[3]) &&
                    std::isdigit(potentialChipName[4]) && std::isdigit(potentialChipName[5]))
                {
                    bool chipMismatch = !currentChipName.empty() &&
                        (currentChipName.compare(0, 5, "sl261") == 0
                            ? potentialChipName.compare(0, 5, currentChipName, 0, 5) != 0
                            : potentialChipName != currentChipName);
                    if (chipMismatch) {
                        log(ASTRA_LOG_LEVEL_WARNING) << "TAG file chip name: " << potentialChipName <<
                            " does not match chip in config: " << currentChipName << endLog;
                        continue;
                    }

                    // Extract board name from the part of the filename after the chip name.
                    // e.g. TAG--astra-media-sl2619-coralboard.rootfs-... -> "coralboard"
                    std::size_t boardPos = pos + 6;
                    if (boardPos < filename.size() && filename[boardPos] == '-') {
                        std::string boardRemainder = filename.substr(boardPos + 1);
                        std::size_t dotPos = boardRemainder.find('.');
                        result.boardName = boardRemainder.substr(0, dotPos);
                    }

                    if (potentialChipName == "sl1680") {
                        result.chipName = potentialChipName;
                        result.secureBootVersion = ASTRA_SECURE_BOOT_V3;
                        result.memoryLayout = ASTRA_MEMORY_LAYOUT_4GB;
                        result.memoryDDRType = ASTRA_MEMORY_DDR_TYPE_LPDDR4X;
                        result.found = true;
                        log(ASTRA_LOG_LEVEL_INFO) << "Detected image is for chip: " << result.chipName << endLog;
                    }
                    else if (potentialChipName == "sl1640") {
                        result.chipName = potentialChipName;
                        result.secureBootVersion = ASTRA_SECURE_BOOT_V3;
                        result.memoryLayout = ASTRA_MEMORY_LAYOUT_2GB;
                        result.memoryDDRType = ASTRA_MEMORY_DDR_TYPE_LPDDR4;
                        result.found = true;
                        log(ASTRA_LOG_LEVEL_INFO) << "Detected image is for chip: " << result.chipName << endLog;
                    }
                    else if (potentialChipName == "sl1620") {
                        result.chipName = potentialChipName;
                        result.secureBootVersion = ASTRA_SECURE_BOOT_V3;
                        result.memoryLayout = ASTRA_MEMORY_LAYOUT_2GB;
                        result.memoryDDRType = ASTRA_MEMORY_DDR_TYPE_DDR4;
                        result.found = true;
                        log(ASTRA_LOG_LEVEL_INFO) << "Detected image is for chip: " << result.chipName << endLog;
                    }
                    else if (potentialChipName.compare(0, 5, "sl261") == 0) {
                        result.chipName = potentialChipName;
                        result.secureBootVersion = ASTRA_SECURE_BOOT_V3;
                        result.memoryLayout = ASTRA_MEMORY_LAYOUT_2GB;
                        // Set board-specific DDR type defaults
                        if (result.boardName == "coralboard") {
                            result.memoryDDRType = ASTRA_MEMORY_DDR_TYPE_DDR4X16;
                        } else {
                            result.memoryDDRType = ASTRA_MEMORY_DDR_TYPE_DDR4;
                        }
                        result.found = true;
                        log(ASTRA_LOG_LEVEL_INFO) << "Detected image is for chip: " << result.chipName << endLog;
                    }
                    break;
                }
            }
        }
    }

    return result;
}
