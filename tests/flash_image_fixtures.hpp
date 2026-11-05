// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#pragma once

#include <set>

#include "boot_image_fixtures.hpp"
#include "emmc_flash_image.hpp"
#include "nand_flash_image.hpp"
#include "spi_flash_image.hpp"
#include "log_fixture.hpp"

namespace test {

using ManifestMaps = std::vector<Manifest>;

inline std::unique_ptr<ManifestMaps> Maps(ManifestMaps maps)
{
    return std::make_unique<ManifestMaps>(std::move(maps));
}

inline SpiFlashImage Spi(const std::filesystem::path &path,
    std::unique_ptr<ManifestMaps> maps = nullptr, const std::string &chip = "sl1680",
    AstraUbootVersion version = ASTRA_UBOOT_VERSION_2025_01)
{
    return SpiFlashImage(path.string(), "boot-id", chip, "rdk", ASTRA_SECURE_BOOT_V3,
        ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_DDR4, version, true, std::move(maps));
}

inline NandFlashImage Nand(const std::filesystem::path &path,
    std::unique_ptr<ManifestMaps> maps = nullptr, const std::string &chip = "sl2619")
{
    return NandFlashImage(path.string(), "boot-id", chip, "rdk", ASTRA_SECURE_BOOT_V2,
        ASTRA_MEMORY_LAYOUT_1GB, ASTRA_MEMORY_DDR_TYPE_DDR3, false, std::move(maps));
}

inline EmmcFlashImage Emmc(const std::filesystem::path &path, const std::string &chip = "sl2619")
{
    return EmmcFlashImage(path.string(), "boot-id", chip, "rdk", ASTRA_SECURE_BOOT_V2,
        ASTRA_MEMORY_LAYOUT_1GB, ASTRA_MEMORY_DDR_TYPE_DDR3, false, nullptr);
}

inline void RequireLoadError(FlashImage &image, const std::string &expected)
{
    TempDirectory logs;
    const auto path = logs.Path() / "load.log";
    {
        LogSession session(path.string(), ASTRA_LOG_LEVEL_ERROR);
        REQUIRE(image.Load() == -1);
    }
    REQUIRE(image.GetLoadError().find(expected) != std::string::npos);
    REQUIRE(!image.GetLoadError().empty());
    const auto message = ReadFile(path, std::ios::in);
    REQUIRE(message.find("[ERROR]") != std::string::npos);
    REQUIRE(message.find(image.GetLoadError()) != std::string::npos);
}

inline void RequirePayloads(const FlashImage &image, AstraImageType type,
    const std::vector<std::string> &expected, bool ordered = true)
{
    std::vector<std::string> names;
    for (auto payload : image.GetImages()) {
        REQUIRE(payload.GetImageType() == type);
        REQUIRE(payload.Load() == 0);
        names.push_back(payload.GetName());
    }
    if (ordered) {
        REQUIRE(names == expected);
    } else {
        REQUIRE(std::multiset<std::string>(names.begin(), names.end()) ==
            std::multiset<std::string>(expected.begin(), expected.end()));
    }
}

class WorkingDirectory
{
public:
    explicit WorkingDirectory(const std::filesystem::path &path) : m_previous(std::filesystem::current_path())
    {
        std::filesystem::current_path(path);
    }
    ~WorkingDirectory()
    {
        std::error_code error;
        std::filesystem::current_path(m_previous, error);
        if (error) {
            ++cleanupFailures;
            std::cerr << "Failed to restore working directory: " << error.message() << '\n';
        }
    }
    WorkingDirectory(const WorkingDirectory &) = delete;
    WorkingDirectory &operator=(const WorkingDirectory &) = delete;

private:
    std::filesystem::path m_previous;
};

inline std::string TagName(const std::string &chip, const std::string &board = "rdk")
{
    return "TAG--astra-media-" + chip + "-" + board + ".rootfs-test";
}

} // namespace test
