// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <filesystem>
#include <string>

#include "spi_flash_image.hpp"
#include "astra_log.hpp"

int SpiFlashImage::Load()
{
    ASTRA_LOG;

    int ret = 0;

    // Allow spi command values from config override the default values
    if (m_config.find("read_address") != m_config.end()) {
        m_readAddress = m_config["read_address"];
    }
    if (m_config.find("write_first_copy_address") != m_config.end()) {
        m_writeFirstCopyAddress = m_config["write_first_copy_address"];
    }
    if (m_config.find("write_second_copy_address") != m_config.end()) {
        m_writeSecondCopyAddress = m_config["write_second_copy_address"];
    }
    if (m_config.find("write_length") != m_config.end()) {
        m_writeLength = m_config["write_length"];
    }
    if (m_config.find("erase_first_start_address") != m_config.end()) {
        m_eraseFirstStartAddress = m_config["erase_first_start_address"];
    }
    if (m_config.find("erase_first_end_address") != m_config.end()) {
        m_eraseFirstEndAddress = m_config["erase_first_end_address"];
    }
    if (m_config.find("erase_second_start_address") != m_config.end()) {
        m_eraseSecondStartAddress = m_config["erase_second_start_address"];
    }
    if (m_config.find("erase_second_end_address") != m_config.end()) {
        m_eraseSecondEndAddress = m_config["erase_second_end_address"];
    }

    std::string imageFile;
    if (m_config.find("image_file") != m_config.end()) {
        imageFile = m_config["image_file"];
        std::string fullImagePath = m_imagePath + "/" + imageFile;
        if (std::filesystem::exists(fullImagePath)) {
            m_images.push_back(Image(fullImagePath, ASTRA_IMAGE_TYPE_UPDATE_SPI));
            m_finalImage = imageFile;
        } else {
            return -1;
        }
    } else {
        imageFile = std::filesystem::path(m_imagePath).filename().string();
        if (std::filesystem::exists(m_imagePath)) {
            m_images.push_back(Image(m_imagePath, ASTRA_IMAGE_TYPE_UPDATE_SPI));
            m_finalImage = imageFile;
        } else {
            return -1;
        }
    }

    // Flash primary and secondary copies of the SPI U-Boot image
    m_flashCommand = "usbload " + imageFile + " " + m_readAddress + "; spinit; erase " 
        + m_eraseFirstStartAddress + " " + m_eraseFirstEndAddress + "; cp.b " + m_readAddress + " " + m_writeFirstCopyAddress
        + " " + m_writeLength + "; erase " + m_eraseSecondStartAddress + " " + m_eraseSecondEndAddress
        + "; cp.b " + m_readAddress + " " + m_writeSecondCopyAddress + " " + m_writeLength + ";" + m_resetCommand;
    m_resetWhenComplete = true;

    return ret;
}