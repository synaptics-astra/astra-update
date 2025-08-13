// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <vector>
#include <memory>
#include "astra_boot_image.hpp"

class BootImageCollection
{
public:
    BootImageCollection(std::string path) : m_path{path}
    {}
    ~BootImageCollection();

    void Load();

    std::vector<std::tuple<uint16_t, uint16_t>> GetDeviceIDs() const;
    AstraBootImage &GetBootImage(std::string id) const;

    std::vector<std::shared_ptr<AstraBootImage>> GetBootImagesForChip(std::string chipName,
        AstraSecureBootVersion secureBoot, AstraMemoryLayout memoryLayout, AstraMemoryDDRType memoryDDRType,
        std::string boardName) const;

private:
    std::string m_path;
    std::vector<std::shared_ptr<AstraBootImage>> m_bootImages;

    void LoadBootImage(const std::filesystem::path &path);

};