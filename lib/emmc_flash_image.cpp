// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "image.hpp"
#include "emmc_flash_image.hpp"
#include "astra_log.hpp"

int EmmcFlashImage::Load()
{
    ASTRA_LOG;

    int ret = 0;

    if (!m_imagePath.empty() && m_imagePath.back() == '/') {
        m_imagePath.erase(m_imagePath.size() - 1);
    }

    if (std::filesystem::exists(m_imagePath) && std::filesystem::is_directory(m_imagePath)) {
        std::string directoryName = std::filesystem::path(m_imagePath).filename().string();
        m_flashCommand = "l2emmc " + directoryName + m_resetCommand;
        m_resetWhenComplete = true;
        for (const auto& entry : std::filesystem::directory_iterator(m_imagePath)) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Found file: " << entry.path() << endLog;
            std::string filename = entry.path().filename().string();
            if ((filename.find("emmc") != std::string::npos) ||
                (filename.find("subimg") != std::string::npos))
            {
                m_images.push_back(std::move(Image(entry.path().string(), ASTRA_IMAGE_TYPE_UPDATE_EMMC)));
            } else if ((filename.find("TAG--") != std::string::npos) && (filename.find("astra") != std::string::npos)) {
                // Yocto builds create a TAG file in the image directory. The name of the file
                // contains the chip name and image name. We use this to determine the chip name
                // and secure boot version if not provided in the config.

                std::size_t pos = filename.find("sl");
                if (pos != std::string::npos && pos + 6 <= filename.size()) {
                    std::string potentialChipName = filename.substr(pos, 6);
                    if (potentialChipName.size() == 6 && std::isdigit(potentialChipName[2]) && std::isdigit(potentialChipName[3]) &&
                        std::isdigit(potentialChipName[4]) && std::isdigit(potentialChipName[5]))
                    {
                        if (!m_chipName.empty() && potentialChipName != m_chipName) {
                            log(ASTRA_LOG_LEVEL_WARNING) << "Image tag chip name: " << potentialChipName <<
                                "chip name in config" << m_chipName << endLog;
                            continue;
                        }
                        if (m_chipName.empty() && potentialChipName == "sl1680") {
                            m_chipName = potentialChipName;
                            m_secureBootVersion = ASTRA_SECURE_BOOT_V3;
                            m_memoryLayout = ASTRA_MEMORY_LAYOUT_4GB;
                            log(ASTRA_LOG_LEVEL_INFO) << "Detected that this image is for chip: " << m_chipName << endLog;
                        }
                        else if (m_chipName.empty() && potentialChipName == "sl1640") {
                            m_chipName = potentialChipName;
                            m_secureBootVersion = ASTRA_SECURE_BOOT_V3;
                            m_memoryLayout = ASTRA_MEMORY_LAYOUT_2GB;
                            log(ASTRA_LOG_LEVEL_INFO) << "Detected that this image is for chip: " << m_chipName << endLog;
                        }
                        else if (m_chipName.empty() && potentialChipName == "sl1620") {
                            m_chipName = potentialChipName;
                            m_secureBootVersion = ASTRA_SECURE_BOOT_V3;
                            m_memoryLayout = ASTRA_MEMORY_LAYOUT_2GB;
                            log(ASTRA_LOG_LEVEL_INFO) << "Detected that this image is for chip: " << m_chipName << endLog;
                        }
                    }
                }
            }
        }
    }

    ParseEmmcImageList();

    return ret;
}

void EmmcFlashImage::ParseEmmcImageList()
{
    ASTRA_LOG;

    std::string emmcPartImagePath;
    for (const auto& image : m_images) {
        if (image.GetName() == "emmc_image_list") {
            emmcPartImagePath = image.GetPath();
            break;
        }
    }

    std::ifstream file(emmcPartImagePath);
    std::string line;
    std::string lastEntryName;

    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string name;
        if (std::getline(iss, name, ',')) {
            name.erase(name.find_last_not_of(",") + 1);
            lastEntryName = name;
        }
    }

    m_finalImage = lastEntryName;
    log(ASTRA_LOG_LEVEL_DEBUG) << "Final image: " << m_finalImage << endLog;
}