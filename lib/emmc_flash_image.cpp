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
        m_flashCommand = "l2emmc " + directoryName;
        if (m_resetWhenComplete) {
            m_flashCommand += m_resetCommand;
        }
        for (const auto& entry : std::filesystem::directory_iterator(m_imagePath)) {
            log(ASTRA_LOG_LEVEL_DEBUG) << "Found file: " << entry.path() << endLog;
            std::string filename = entry.path().filename().string();
            if ((filename.find("emmc") != std::string::npos) ||
                (filename.find("subimg") != std::string::npos))
            {
                m_images.push_back(std::move(Image(entry.path().string(), ASTRA_IMAGE_TYPE_UPDATE_EMMC)));
            }
            // TAG-- files are handled separately by DetectChipFromTagFile() below.
        }
    }


    // Detect chip info from TAG file if chip name not already set
    if (m_chipName.empty()) {
        try {
            ChipDetectionResult detection = DetectChipFromTagFile(m_imagePath, m_chipName);
            if (detection.found) {
                m_chipName = detection.chipName;
                m_secureBootVersion = detection.secureBootVersion;
                m_memoryLayout = detection.memoryLayout;
            }
        } catch (const std::exception& e) {
            log(ASTRA_LOG_LEVEL_WARNING) << "Failed to detect chip from TAG file: " << e.what() << endLog;
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
            if (name != "erase" && name != "format") {
                // Ignore erase and format operations since they do not send images to the device.
                // When this is the final operation, waiting for a image request will cause a timeout and
                // incorrectly report a failure.
                lastEntryName = name;
            }
        }
    }

    m_finalImage = lastEntryName;
    log(ASTRA_LOG_LEVEL_DEBUG) << "Final image: " << m_finalImage << endLog;
}