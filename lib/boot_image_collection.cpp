// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include "boot_image_collection.hpp"
#include "astra_boot_image.hpp"
#include "image.hpp"
#include "astra_log.hpp"

namespace {

unsigned int MemoryLayoutMegabytes(AstraMemoryLayout layout)
{
    switch (layout) {
    case ASTRA_MEMORY_LAYOUT_512MB: return 512;
    case ASTRA_MEMORY_LAYOUT_1GB: return 1024;
    case ASTRA_MEMORY_LAYOUT_2GB: return 2048;
    case ASTRA_MEMORY_LAYOUT_3GB: return 3072;
    case ASTRA_MEMORY_LAYOUT_4GB: return 4096;
    default: throw std::invalid_argument("Invalid memory layout for boot image selection");
    }
}

}

void BootImageCollection::LoadBootImage(const std::filesystem::path &path)
{
    ASTRA_LOG;

    if (std::filesystem::exists(path / "manifest.yaml")) {
        AstraBootImage bootImage{path.string()};

        if(bootImage.Load()) {
            m_bootImages.push_back(std::make_shared<AstraBootImage>(bootImage));
        }
    }
}

void BootImageCollection::Load()
{
    ASTRA_LOG;

    log(ASTRA_LOG_LEVEL_DEBUG) << "Loading boot images from " << m_path << endLog;

    std::filesystem::path dir(m_path);

    if (std::filesystem::exists(dir)) {
        if (std::filesystem::is_directory(dir)) {
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (std::filesystem::is_directory(entry.path())) {
                    LoadBootImage(entry.path());
                }
            }
        } else {
            LoadBootImage(dir);
        }
    } else {
        throw std::invalid_argument("Boot Images directory " + m_path + " not found");
    }
}

std::vector<std::tuple<uint16_t, uint16_t>> BootImageCollection::GetDeviceIDs() const
{
    ASTRA_LOG;

    std::vector<std::tuple<uint16_t, uint16_t>> deviceIds;

    for (const auto& bootImages : m_bootImages) {
        deviceIds.push_back(std::make_tuple(bootImages->GetVendorId(), bootImages->GetProductId()));
    }

    return deviceIds;
}

AstraBootImage &BootImageCollection::GetBootImage(std::string id) const
{
    ASTRA_LOG;

    for (const auto& bootImage : m_bootImages) {
        if (bootImage->GetID() == id) {
            return *bootImage;
        }
    }

    throw std::runtime_error("Boot Images not found");
}

std::vector<std::shared_ptr<AstraBootImage>> BootImageCollection::GetBootImagesForChip(std::string chipName,
    AstraSecureBootVersion secureBoot, AstraMemoryLayout memoryLayout, AstraMemoryDDRType memoryDDRType,
    std::string boardName, std::optional<FlashImageType> updateType) const
{
    ASTRA_LOG;

    const auto targetMemoryMegabytes = MemoryLayoutMegabytes(memoryLayout);

    // For SL261x variants (where x > 0), collect candidate chip names: exact match first, then series fallback.
    std::vector<std::string> chipNamesToTry = { chipName };
    if (chipName.size() == 6 && chipName.compare(0, 5, "sl261") == 0 && chipName[5] > '0') {
        chipNamesToTry.push_back("sl2610");
    }

    std::vector<std::shared_ptr<AstraBootImage>> bootImages;

    for (const auto& candidateChipName : chipNamesToTry) {
        for (const auto& bootImage : m_bootImages) {
            if (bootImage->GetChipName() == candidateChipName && bootImage->GetSecureBootVersion() == secureBoot
              && MemoryLayoutMegabytes(bootImage->GetMemoryLayout()) <= targetMemoryMegabytes)
            {
                const bool boardImageMatch = boardName.empty() || boardName == "any"
                    || bootImage->GetBoardName() == boardName;
                const bool ddrTypeMatch = memoryDDRType == ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED
                    || bootImage->GetMemoryDDRType() == memoryDDRType;

                const bool xspiMatch = updateType != FLASH_IMAGE_TYPE_SPI || bootImage->GetXspiSupport() != false;
                if (boardImageMatch && ddrTypeMatch && xspiMatch) {
                    bootImages.push_back(bootImage);
                }
            }
        }

        // If we found matches with the exact chip name, do not try the series fallback.
        if (!bootImages.empty()) {
            if (updateType == FLASH_IMAGE_TYPE_NAND) {
                bootImages.erase(std::remove_if(bootImages.begin(), bootImages.end(),
                    [](const std::shared_ptr<AstraBootImage> &image) { return !image->GetNandSupport(); }),
                    bootImages.end());
            }
            if (updateType.has_value()) {
                // Unspecified manifests remain usable; prefer explicit XSPI
                // support for SPI, and false/unspecified for other updates.
                const auto preferred = [updateType](const std::shared_ptr<AstraBootImage> &image) {
                    return updateType == FLASH_IMAGE_TYPE_SPI ? image->GetXspiSupport() == true
                        : image->GetXspiSupport() != true;
                };
                if (std::any_of(bootImages.begin(), bootImages.end(), preferred)) {
                    bootImages.erase(std::remove_if(bootImages.begin(), bootImages.end(),
                        [&](const std::shared_ptr<AstraBootImage> &image) { return !preferred(image); }),
                        bootImages.end());
                }
            }
            break;
        }
    }

    return bootImages;
}

BootImageCollection::~BootImageCollection()
{
    ASTRA_LOG;
}