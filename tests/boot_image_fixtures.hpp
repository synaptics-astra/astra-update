// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#pragma once

#include <iomanip>
#include <map>
#include <sstream>

#include "astra_boot_image.hpp"
#include "test_support.hpp"

namespace test {

using Manifest = std::map<std::string, std::string>;

inline Manifest BootManifest(const std::string &id = "test-image")
{
    return {
        {"id", id}, {"chip", "sl2619"}, {"board", "rdk"}, {"secure_boot", "genx"},
        {"memory_layout", "2gb"}, {"console", "uart"}, {"uenv_support", "false"},
        {"vendor_id", "06CB"}, {"product_id", "019E"}, {"uboot", "suboot"},
        {"uboot_version", "2025.01"}
    };
}

inline std::string ManifestYaml(const Manifest &fields)
{
    std::ostringstream yaml;
    for (const auto &field : fields) {
        yaml << field.first << ": " << std::quoted(field.second) << '\n';
    }
    return yaml.str();
}

inline std::filesystem::path WriteBootManifest(const TempDirectory &directory,
    const Manifest &fields, const std::filesystem::path &relativeDirectory = {})
{
    directory.Write(relativeDirectory / "manifest.yaml", ManifestYaml(fields));
    return directory.Path() / relativeDirectory;
}

struct LayoutCase {
    const char *name;
    AstraMemoryLayout value;
};

inline constexpr LayoutCase layouts[] = {
    {"1gb", ASTRA_MEMORY_LAYOUT_1GB}, {"2gb", ASTRA_MEMORY_LAYOUT_2GB},
    {"3gb", ASTRA_MEMORY_LAYOUT_3GB}, {"4gb", ASTRA_MEMORY_LAYOUT_4GB},
    {"512mb", ASTRA_MEMORY_LAYOUT_512MB}
};

struct DDRCase {
    const char *name;
    AstraMemoryDDRType value;
};

inline constexpr DDRCase ddrTypes[] = {
    {"ddr3", ASTRA_MEMORY_DDR_TYPE_DDR3}, {"ddr4", ASTRA_MEMORY_DDR_TYPE_DDR4},
    {"lpddr4", ASTRA_MEMORY_DDR_TYPE_LPDDR4}, {"lpddr4x", ASTRA_MEMORY_DDR_TYPE_LPDDR4X},
    {"ddr4x16", ASTRA_MEMORY_DDR_TYPE_DDR4X16}
};

} // namespace test
