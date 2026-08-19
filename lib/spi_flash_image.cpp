// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <array>
#include <filesystem>
#include <string>

#include "spi_flash_image.hpp"
#include "astra_log.hpp"

void SpiFlashImage::ParseSpiFlashConfig(const std::map<std::string, std::string> &config, std::string imageFile)
{
    ASTRA_LOG;
    m_spiImageOverrides.push_back({imageFile, config});
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
                        m_loadError = "SPI image file not found: " + fullImagePath;
                        log(ASTRA_LOG_LEVEL_ERROR) << m_loadError << endLog;
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
                        m_loadError = "SPI image file not found: " + fullImagePath;
                        log(ASTRA_LOG_LEVEL_ERROR) << m_loadError << endLog;
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
            m_spiImageOverrides.push_back({imageFile, {}});
        } else {
            m_loadError = "SPI image path does not exist: " + m_imagePath;
            log(ASTRA_LOG_LEVEL_ERROR) << m_loadError << endLog;
            return -1;
        }
    }

    if (m_spiImageOverrides.empty()) {
        m_loadError = "No SPI images configured - check that the manifest has an image_file entry";
        log(ASTRA_LOG_LEVEL_ERROR) << m_loadError << endLog;
        return -1;
    }

    // Reject manifest values that would be unsafe or malformed once
    // concatenated into the U-Boot flash command.
    if (!ValidateOverrides()) {
        return -1;
    }

    // Flash primary and secondary copies of the SPI U-Boot image
    BuildFlashCommand();

    return ret;
}

bool SpiFlashImage::ValidateOverrides()
{
    ASTRA_LOG;

    // Every manifest key whose value is substituted into the flash command.
    static const std::array<const char *, 8> kAddressKeys = {
        "read_address",
        "write_first_copy_address",
        "write_second_copy_address",
        "write_length",
        "erase_first_start_address",
        "erase_first_length",
        "erase_second_start_address",
        "erase_second_length",
    };

    for (const auto &override : m_spiImageOverrides) {
        if (!IsSafeUbootFilename(override.imageFile)) {
            m_loadError = "Unsafe image_file in SPI manifest: '" + override.imageFile + "'";
            log(ASTRA_LOG_LEVEL_ERROR) << m_loadError << endLog;
            return false;
        }

        for (const char *key : kAddressKeys) {
            const auto it = override.configOverrides.find(key);
            if (it == override.configOverrides.end()) {
                continue;
            }

            if (!IsSafeUbootNumber(it->second)) {
                m_loadError = std::string("Unsafe ") + key + " in SPI manifest for "
                    + override.imageFile + ": '" + it->second + "'";
                log(ASTRA_LOG_LEVEL_ERROR) << m_loadError << endLog;
                return false;
            }
        }
    }

    return true;
}

void SpiFlashImage::OnUbootVersionChanged()
{
    BuildFlashCommand();
}

void SpiFlashImage::BuildFlashCommand()
{
    ASTRA_LOG;

    m_flashCommand.clear();

    if (m_ubootVersion == ASTRA_UBOOT_VERSION_2019_10) {
        log(ASTRA_LOG_LEVEL_DEBUG) << "Using U-Boot 2019.10 SPI flash command sequence" << endLog;
        for (const auto &override : m_spiImageOverrides) {
            SpiImageConfig cfg(m_chipName, m_ubootVersion);
            cfg.imageFile = override.imageFile;
            if (auto it = override.configOverrides.find("read_address"); it != override.configOverrides.end()) cfg.readAddress = it->second;
            if (auto it = override.configOverrides.find("write_first_copy_address"); it != override.configOverrides.end()) cfg.writeFirstCopyAddress = it->second;
            if (auto it = override.configOverrides.find("write_second_copy_address"); it != override.configOverrides.end()) cfg.writeSecondCopyAddress = it->second;
            if (auto it = override.configOverrides.find("write_length"); it != override.configOverrides.end()) cfg.writeLength = it->second;
            if (auto it = override.configOverrides.find("erase_first_start_address"); it != override.configOverrides.end()) cfg.eraseFirstStartAddress = it->second;
            if (auto it = override.configOverrides.find("erase_first_length"); it != override.configOverrides.end()) cfg.eraseFirstLength = it->second;
            if (auto it = override.configOverrides.find("erase_second_start_address"); it != override.configOverrides.end()) cfg.eraseSecondStartAddress = it->second;
            if (auto it = override.configOverrides.find("erase_second_length"); it != override.configOverrides.end()) cfg.eraseSecondLength = it->second;
            m_flashCommand += "usbload " + cfg.imageFile + " " + cfg.readAddress + "; spinit; erase "
                + cfg.eraseFirstStartAddress + " " + cfg.eraseFirstLength + "; cp.b "
                + cfg.readAddress + " " + cfg.writeFirstCopyAddress
                + " " + cfg.writeLength + "; erase "
                + cfg.eraseSecondStartAddress + " " + cfg.eraseSecondLength
                + "; cp.b " + cfg.readAddress + " " + cfg.writeSecondCopyAddress
                + " " + cfg.writeLength + "; ";
        }
        log(ASTRA_LOG_LEVEL_DEBUG) << "Flash command: " << m_flashCommand << endLog;
    } else {
        log(ASTRA_LOG_LEVEL_DEBUG) << "Using U-Boot 2025.01 SPI flash command sequence" << endLog;
        for (const auto &override : m_spiImageOverrides) {
            SpiImageConfig cfg(m_chipName, m_ubootVersion);
            cfg.imageFile = override.imageFile;
            if (auto it = override.configOverrides.find("read_address"); it != override.configOverrides.end()) cfg.readAddress = it->second;
            if (auto it = override.configOverrides.find("write_first_copy_address"); it != override.configOverrides.end()) cfg.writeFirstCopyAddress = it->second;
            if (auto it = override.configOverrides.find("write_second_copy_address"); it != override.configOverrides.end()) cfg.writeSecondCopyAddress = it->second;
            if (auto it = override.configOverrides.find("write_length"); it != override.configOverrides.end()) cfg.writeLength = it->second;
            if (auto it = override.configOverrides.find("erase_first_start_address"); it != override.configOverrides.end()) cfg.eraseFirstStartAddress = it->second;
            if (auto it = override.configOverrides.find("erase_first_length"); it != override.configOverrides.end()) cfg.eraseFirstLength = it->second;
            if (auto it = override.configOverrides.find("erase_second_start_address"); it != override.configOverrides.end()) cfg.eraseSecondStartAddress = it->second;
            if (auto it = override.configOverrides.find("erase_second_length"); it != override.configOverrides.end()) cfg.eraseSecondLength = it->second;
            m_flashCommand += "usbload " + cfg.imageFile + " " + cfg.readAddress + "; sf probe; sf erase " + cfg.eraseFirstStartAddress + " " + cfg.eraseFirstLength
                + "; sf write " + cfg.readAddress + " " + cfg.writeFirstCopyAddress + " " + cfg.writeLength + "; sf erase " + cfg.eraseSecondStartAddress
                + " " + cfg.eraseSecondLength + "; sf write " + cfg.readAddress + " " + cfg.writeSecondCopyAddress + " " + cfg.writeLength + "; ";
        }
        log(ASTRA_LOG_LEVEL_DEBUG) << "Flash command: " << m_flashCommand << endLog;
    }

    if (m_resetWhenComplete) {
        m_flashCommand += m_resetCommand;
    }
}