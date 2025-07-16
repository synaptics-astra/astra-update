// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <filesystem>
#include <string>

#include "spi_flash_image.hpp"
#include "astra_log.hpp"

void SpiFlashImage::ParseSpiFlashConfig(const std::map<std::string, std::string> &config, std::string imageFile)
{
    ASTRA_LOG;

    SpiImageConfig spiConfig;

    spiConfig.imageFile = imageFile;

    // Allow spi command values from config override the default values
    if (auto it = config.find("read_address"); it != config.end()) {
        spiConfig.readAddress = it->second;
    }

    if (auto it = config.find("write_first_copy_address"); it != config.end()) {
        spiConfig.writeFirstCopyAddress = it->second;
    }

    if (auto it = config.find("write_second_copy_address"); it != config.end()) {
        spiConfig.writeSecondCopyAddress = it->second;
    }

    if (auto it = config.find("write_length"); it != config.end()) {
        spiConfig.writeLength = it->second;
    }

    if (auto it = config.find("erase_first_start_address"); it != config.end()) {
        spiConfig.eraseFirstStartAddress = it->second;
    }

    if (auto it = config.find("erase_first_length"); it != config.end()) {
        spiConfig.eraseFirstLength = it->second;
    }

    if (auto it = config.find("erase_second_start_address"); it != config.end()) {
        spiConfig.eraseSecondStartAddress = it->second;
    }

    if (auto it = config.find("erase_second_length"); it != config.end()) {
        spiConfig.eraseSecondLength = it->second;
    }

    m_spiImageConfigs.push_back(spiConfig);
}

int SpiFlashImage::Load()
{
    ASTRA_LOG;

    int ret = 0;

    std::string imageFile;

    if (m_manifestMaps && !m_manifestMaps->empty()) {
        for (const auto &map : *m_manifestMaps) {
            if (map.find("type") != map.end() && map.at("type") == "config") {
                // If the manifest file contains a config section with an image_file entry,
                // then this is a legacy manifest from before we supported multiple SPI images.
                if (map.find("image_file") != map.end()) {
                    imageFile = map.at("image_file");
                    std::string fullImagePath = m_imagePath + "/" + imageFile;
                    if (std::filesystem::exists(fullImagePath)) {
                        m_images.push_back(Image(fullImagePath, ASTRA_IMAGE_TYPE_UPDATE_SPI));
                        m_finalImage = imageFile;
                        ParseSpiFlashConfig(map, imageFile);
                    } else {
                        return -1;
                    }
                }
            } else if (map.find("type") != map.end() && map.at("type") == "image") {
                // If the manifest file contains image sections, then we will use that to load the SPI images.
                if (map.find("image_file") != map.end()) {
                    imageFile = map.at("image_file");
                    std::string fullImagePath = m_imagePath + "/" + imageFile;
                    if (std::filesystem::exists(fullImagePath)) {
                        m_images.push_back(Image(fullImagePath, ASTRA_IMAGE_TYPE_UPDATE_SPI));
                        m_finalImage = imageFile;
                        ParseSpiFlashConfig(map, imageFile);
                    } else {
                        return -1;
                    }
                }
            }
        }
    } else {
        imageFile = std::filesystem::path(m_imagePath).filename().string();
        if (std::filesystem::exists(m_imagePath)) {
            m_images.push_back(Image(m_imagePath, ASTRA_IMAGE_TYPE_UPDATE_SPI));
            m_finalImage = imageFile;

            // If no manifest file was provided, then we will use the default SPI flash configuration.
            SpiImageConfig spiConfig;
            spiConfig.imageFile = imageFile;
            m_spiImageConfigs.push_back(spiConfig);
        } else {
            return -1;
        }
    }

    // Flash primary and secondary copies of the SPI U-Boot image
    for (const auto &imageConfig : m_spiImageConfigs) {
        m_flashCommand += "usbload " + imageConfig.imageFile + " " + imageConfig.readAddress + "; spinit; erase "
            + imageConfig.eraseFirstStartAddress + " " + imageConfig.eraseFirstLength + "; cp.b "
            + imageConfig.readAddress + " " + imageConfig.writeFirstCopyAddress
            + " " + imageConfig.writeLength + "; erase "
            + imageConfig.eraseSecondStartAddress + " " + imageConfig.eraseSecondLength
            + "; cp.b " + imageConfig.readAddress + " " + imageConfig.writeSecondCopyAddress
            + " " + imageConfig.writeLength + "; ";
    }

    if (m_resetWhenComplete) {
        m_flashCommand += m_resetCommand;
    }

    return ret;
}