// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <fstream>
#include <string>
#include <filesystem>

enum AstraSecureBootVersion {
    ASTRA_SECURE_BOOT_V2,
    ASTRA_SECURE_BOOT_V3,
};


enum AstraMemoryLayout {
    ASTRA_MEMORY_LAYOUT_1GB = 0,
    ASTRA_MEMORY_LAYOUT_2GB = 1,
    ASTRA_MEMORY_LAYOUT_3GB = 2,
    ASTRA_MEMORY_LAYOUT_4GB = 3,
};

enum AstraMemoryDDRType {
    ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED = 0,
    ASTRA_MEMORY_DDR_TYPE_DDR3 = 1,
    ASTRA_MEMORY_DDR_TYPE_DDR4 = 2,
    ASTRA_MEMORY_DDR_TYPE_LPDDR4 = 3,
    ASTRA_MEMORY_DDR_TYPE_LPDDR4X = 4,
    ASTRA_MEMORY_DDR_TYPE_DDR4X16 = 5,
};

enum AstraImageType {
    ASTRA_IMAGE_TYPE_BOOT,
    ASTRA_IMAGE_TYPE_UPDATE_EMMC,
    ASTRA_IMAGE_TYPE_UPDATE_SPI,
    ASTRA_IMAGE_TYPE_UPDATE_NAND,
};

class Image
{
public:
    Image(std::string imagePath, AstraImageType imageType) : m_imagePath{imagePath}, m_imageSize{0},
        m_imageType{imageType}, m_fp{nullptr}
    {
        m_imageName = std::filesystem::path(m_imagePath).filename().string();
    }
    Image(const Image &other) : m_imagePath{other.m_imagePath}, m_imageName{other.m_imageName},
        m_imageSize{other.m_imageSize}, m_imageType{other.m_imageType}, m_fp{other.m_fp}
    {}
    ~Image();

    Image &operator=(const Image &other)
    {
        m_imagePath = other.m_imagePath;
        m_imageName = other.m_imageName;
        m_imageSize = other.m_imageSize;
        m_imageType = other.m_imageType;
        m_fp = other.m_fp;
        return *this;
    }

    int Load();

    std::string GetName() const { return m_imageName; }
    std::string GetPath() const { return m_imagePath; }
    int GetDataBlock(uint8_t *data, size_t size);
    size_t GetSize() const { return m_imageSize; }
    AstraImageType GetImageType() const { return m_imageType; }

private:
    std::string m_imagePath;
    std::string m_imageName;
    size_t m_imageSize;
    AstraImageType m_imageType;

    FILE *m_fp;
};

static std::string AstraSecureBootVersionToString(AstraSecureBootVersion version)
{
    switch (version) {
        case ASTRA_SECURE_BOOT_V2:
            return "gen2";
        case ASTRA_SECURE_BOOT_V3:
            return "genx";
        default:
            return "unknown";
    }
}

static std::string AstraMemoryLayoutToString(AstraMemoryLayout memoryLayout)
{
    switch (memoryLayout) {
        case ASTRA_MEMORY_LAYOUT_1GB:
            return "1GB";
        case ASTRA_MEMORY_LAYOUT_2GB:
            return "2GB";
        case ASTRA_MEMORY_LAYOUT_3GB:
            return "3GB";
        case ASTRA_MEMORY_LAYOUT_4GB:
            return "4GB";
        default:
            return "unknown";
    }
}

static std::string AstraMemoryDDRTypeToString(AstraMemoryDDRType ddrType)
{
    switch (ddrType) {
        case ASTRA_MEMORY_DDR_TYPE_DDR3:
            return "DDR3";
        case ASTRA_MEMORY_DDR_TYPE_DDR4:
            return "DDR4";
        case ASTRA_MEMORY_DDR_TYPE_LPDDR4:
            return "LPDDR4";
        case ASTRA_MEMORY_DDR_TYPE_LPDDR4X:
            return "LPDDR4X";
        case ASTRA_MEMORY_DDR_TYPE_DDR4X16:
            return "DDR4X16";
        default:
            return "not_specified";
    }
}