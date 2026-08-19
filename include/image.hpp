// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <cstdio>
#include <fstream>
#include <memory>
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
    ASTRA_MEMORY_LAYOUT_512MB = 4,
};

enum AstraMemoryDDRType {
    ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED = 0,
    ASTRA_MEMORY_DDR_TYPE_DDR3 = 1,
    ASTRA_MEMORY_DDR_TYPE_DDR4 = 2,
    ASTRA_MEMORY_DDR_TYPE_LPDDR4 = 3,
    ASTRA_MEMORY_DDR_TYPE_LPDDR4X = 4,
    ASTRA_MEMORY_DDR_TYPE_DDR4X16 = 5,
};

enum AstraUbootVersion {
    ASTRA_UBOOT_VERSION_UNKNOWN,
    ASTRA_UBOOT_VERSION_2019_10,
    ASTRA_UBOOT_VERSION_2025_01,
};

enum AstraImageType {
    ASTRA_IMAGE_TYPE_BOOT,
    ASTRA_IMAGE_TYPE_UPDATE_EMMC,
    ASTRA_IMAGE_TYPE_UPDATE_SPI,
    ASTRA_IMAGE_TYPE_UPDATE_NAND,
};

enum AstraTransportType {
    ASTRA_TRANSPORT_USB,
    ASTRA_TRANSPORT_USB_CDC,
};

class Image
{
public:
    Image(std::string imagePath, AstraImageType imageType) : m_imagePath{imagePath}, m_imageSize{0},
        m_imageType{imageType}
    {
        m_imageName = std::filesystem::path(m_imagePath).filename().string();
    }

    // Copy, move and destruction are all correct by default.  The open file
    // is owned by a shared_ptr with an fclose deleter, so copies share the
    // handle and it is closed exactly once.  The hand-written copy operations
    // this replaces copied the raw FILE* while the destructor closed it,
    // which double-closed the handle and left the survivor dangling whenever
    // a loaded Image was copied -- for example when the m_images vector
    // reallocated as update images were appended.

    int Load();

    std::string GetName() const { return m_imageName; }
    std::string GetPath() const { return m_imagePath; }
    int GetDataBlock(uint8_t *data, size_t size);
    size_t GetSize() const { return m_imageSize; }
    AstraImageType GetImageType() const { return m_imageType; }

private:
    struct FileCloser {
        void operator()(FILE *fp) const
        {
            if (fp != nullptr) {
                std::fclose(fp);
            }
        }
    };

    std::string m_imagePath;
    std::string m_imageName;
    size_t m_imageSize;
    AstraImageType m_imageType;

    std::shared_ptr<FILE> m_fp;
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
        case ASTRA_MEMORY_LAYOUT_512MB:
            return "512MB";
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

static std::string AstraTransportToString(AstraTransportType transportType)
{
    switch (transportType) {
        case ASTRA_TRANSPORT_USB:
            return "USB";
        case ASTRA_TRANSPORT_USB_CDC:
            return "USB_CDC";
        default:
            return "unknown";
    }
}