// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <vector>
#include <memory>
#include "astra_boot_image.hpp"
#include "flash_image.hpp"

class BootImageCollection
{
public:
    BootImageCollection(std::string path) : m_path{path}
    {}
    ~BootImageCollection();

    void Load();

    std::vector<std::tuple<uint16_t, uint16_t>> GetDeviceIDs() const;
    AstraBootImage &GetBootImage(std::string id) const;

    // memoryLayout is the target capacity; boot layouts no larger than it are eligible.
    // Omitting updateType preserves the unranked chip/board query.
    std::vector<std::shared_ptr<AstraBootImage>> GetBootImagesForChip(std::string chipName,
        AstraSecureBootVersion secureBoot, AstraMemoryLayout memoryLayout, AstraMemoryDDRType memoryDDRType,
        std::string boardName, std::optional<FlashImageType> updateType = std::nullopt) const;

private:
    std::string m_path;
    std::vector<std::shared_ptr<AstraBootImage>> m_bootImages;

    void LoadBootImage(const std::filesystem::path &path);

};