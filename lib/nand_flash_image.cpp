// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <filesystem>
#include <string>

#include "nand_flash_image.hpp"
#include "astra_log.hpp"

int NandFlashImage::Load()
{
    ASTRA_LOG;

    int ret = 0;
    const std::string defaultImageFile = "uNAND_full.img";
    const std::string defaultReadAddress = "0x10000000";

    m_imageFile.clear();
    m_nandReadAddress.clear();

    if (m_manifestMaps && !m_manifestMaps->empty()) {
        for (const auto &map : *m_manifestMaps) {
            const auto typeIt = map.find("type");
            if (typeIt == map.end()) {
                continue;
            }

            if (typeIt->second == "config" || typeIt->second == "image") {
                if (auto it = map.find("image_file"); it != map.end()) {
                    m_imageFile = it->second;
                }

                if (auto it = map.find("read_address"); it != map.end()) {
                    m_nandReadAddress = it->second;
                }

                break;
            }
        }

        if (m_imageFile.empty()) {
            m_imageFile = defaultImageFile;
        }

        std::string fullImagePath = m_imagePath + "/" + m_imageFile;
        if (!std::filesystem::exists(fullImagePath)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "NAND image file not found: " << fullImagePath << endLog;
            return -1;
        }

        m_images.push_back(Image(fullImagePath, ASTRA_IMAGE_TYPE_UPDATE_NAND));
        m_finalImage = m_imageFile;
    } else {
        m_imageFile = std::filesystem::path(m_imagePath).filename().string();
        if (!std::filesystem::exists(m_imagePath)) {
            log(ASTRA_LOG_LEVEL_ERROR) << "NAND image path does not exist: " << m_imagePath << endLog;
            return -1;
        }

        m_images.push_back(Image(m_imagePath, ASTRA_IMAGE_TYPE_UPDATE_NAND));
        m_finalImage = m_imageFile;
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
    if (m_nandReadAddress.empty()) {
        m_nandReadAddress = defaultReadAddress;
    }

    log(ASTRA_LOG_LEVEL_DEBUG) << "NAND image file: " << m_imageFile << endLog;
    if (!m_nandReadAddress.empty()) {
        log(ASTRA_LOG_LEVEL_DEBUG) << "NAND read address: " << m_nandReadAddress << endLog;
    }

    if (!m_imageFile.empty()) {
        m_flashCommand = "usbload " + m_nandReadAddress + " " + m_imageFile
            + "; m2nand " + m_nandReadAddress;
    } else {
        log(ASTRA_LOG_LEVEL_ERROR) << "NAND image file missing!" << endLog;
        return -1;
    }

    if (m_resetWhenComplete) {
        m_flashCommand += m_resetCommand;
    }

    return ret;
}