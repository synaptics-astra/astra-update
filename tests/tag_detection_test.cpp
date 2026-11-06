// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include "flash_image_fixtures.hpp"

namespace {

void Check(const std::string &chip, const std::string &board,
    AstraMemoryLayout layout, AstraMemoryDDRType ddr, const std::string &current = "")
{
    test::TempDirectory directory;
    directory.Write(test::TagName(chip, board), "");
    const auto result = DetectChipFromTagFile(directory.Path().string(), current);
    REQUIRE(result.found);
    REQUIRE(result.chipName == chip && result.boardName == board);
    REQUIRE(result.secureBootVersion == ASTRA_SECURE_BOOT_V3);
    REQUIRE(result.memoryLayout == layout && result.memoryDDRType == ddr);
}

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    suite.Add("sl1680-defaults", [] {
        Check("sl1680", "rdk", ASTRA_MEMORY_LAYOUT_4GB, ASTRA_MEMORY_DDR_TYPE_LPDDR4X);
    });
    suite.Add("sl1640-defaults", [] {
        Check("sl1640", "rdk", ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_LPDDR4);
    });
    suite.Add("sl1620-defaults", [] {
        Check("sl1620", "rdk", ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_DDR4);
    });
    for (int variant = 0; variant <= 9; ++variant) {
        const auto chip = "sl261" + std::to_string(variant);
        for (const std::string board : {"rdk", "coralboard"}) {
            suite.Add(chip + "-" + board + "-defaults", [=] {
                Check(chip, board, ASTRA_MEMORY_LAYOUT_2GB,
                    board == "coralboard" ? ASTRA_MEMORY_DDR_TYPE_DDR4X16 : ASTRA_MEMORY_DDR_TYPE_DDR4);
            });
        }
    }
    for (const std::string chip : {"sl1680", "sl1640", "sl1620", "sl2619"}) {
        suite.Add("explicit-compatible-chip-" + chip, [=] {
            const auto layout = chip == "sl1680" ? ASTRA_MEMORY_LAYOUT_4GB : ASTRA_MEMORY_LAYOUT_2GB;
            const auto ddr = chip == "sl1680" ? ASTRA_MEMORY_DDR_TYPE_LPDDR4X :
                chip == "sl1640" ? ASTRA_MEMORY_DDR_TYPE_LPDDR4 : ASTRA_MEMORY_DDR_TYPE_DDR4;
            Check(chip, "rdk", layout, ddr, chip);
        });
    }
    for (const std::string current : {"sl2610", "sl2618", "sl2619"}) {
        suite.Add("sl261x-compatible-config-" + current, [=] {
            Check("sl2619", "coralboard", ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_DDR4X16, current);
        });
    }
    for (const std::string current : {"sl1680", "sl2620", "SL2619"}) {
        suite.Add("chip-mismatch-warning-" + current, [=] {
            test::TempDirectory directory;
            directory.Write(test::TagName("sl2619"), "");
            const auto logPath = directory.Path() / "warnings.log";
            {
                test::LogSession session(logPath.string(), ASTRA_LOG_LEVEL_WARNING);
                REQUIRE(!DetectChipFromTagFile(directory.Path().string(), current).found);
            }
            const auto message = test::ReadFile(logPath);
            REQUIRE(message.find("does not match chip in config: " + current) != std::string::npos);
        });
    }
    suite.Add("mismatch-skipped-before-compatible-tag", [] {
        test::TempDirectory directory;
        directory.Write(test::TagName("sl1680"), "");
        directory.Write(test::TagName("sl2619"), "");
        const auto result = DetectChipFromTagFile(directory.Path().string(), "sl2610");
        REQUIRE(result.found && result.chipName == "sl2619" && result.boardName == "rdk");
    });
    const std::vector<std::string> ignored = {
        "ordinary.txt", "TAG--other-sl2619-rdk.rootfs", "astra-sl2619-rdk.rootfs",
        "TAG--astra-media-no-chip", "TAG--astra-media-sl1", "TAG--astra-slx619-rdk",
        "TAG--astra-sl2x19-rdk", "TAG--astra-sl26x9-rdk", "TAG--astra-sl261x-rdk",
        "TAG--astra-SL2619-rdk", "TAG--astra-sl9999-rdk"
    };
    for (size_t i = 0; i < ignored.size(); ++i) {
        suite.Add("ignored-or-unsupported-tag-" + std::to_string(i), [name = ignored[i]] {
            test::TempDirectory directory;
            directory.Write(name, "");
            const auto result = DetectChipFromTagFile(directory.Path().string(), "");
            REQUIRE(!result.found && result.chipName.empty());
            REQUIRE(result.memoryLayout == ASTRA_MEMORY_LAYOUT_2GB);
            REQUIRE(result.memoryDDRType == ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED);
        });
    }
    suite.Add("missing-file-and-empty-directory", [] {
        test::TempDirectory directory;
        REQUIRE(!DetectChipFromTagFile((directory.Path() / "missing").string(), "").found);
        REQUIRE(!DetectChipFromTagFile(directory.Write("file", "").string(), "").found);
        REQUIRE(!DetectChipFromTagFile(directory.Path().string(), "").found);
    });
    for (const std::string suffix : {"", "-custom", ".rootfs", "-multi-word-board.rootfs"}) {
        suite.Add("board-suffix-" + suffix, [=] {
            test::TempDirectory directory;
            directory.Write("TAG--astra-sl2619" + suffix, "");
            const auto result = DetectChipFromTagFile(directory.Path().string(), "");
            REQUIRE(result.found && result.chipName == "sl2619");
            REQUIRE(result.boardName == (suffix == "-custom" ? "custom" :
                suffix == "-multi-word-board.rootfs" ? "multi-word-board" : ""));
            REQUIRE(result.memoryDDRType == ASTRA_MEMORY_DDR_TYPE_DDR4);
        });
    }
    suite.Add("characterization-multiple-compatible-tags-use-first", [] {
        test::TempDirectory directory;
        directory.Write(test::TagName("sl2610", "rdk"), "");
        directory.Write(test::TagName("sl2619", "coralboard"), "");
        const auto result = DetectChipFromTagFile(directory.Path().string(), "");
        REQUIRE(result.found);
        REQUIRE((result.chipName == "sl2610" && result.boardName == "rdk" &&
            result.memoryDDRType == ASTRA_MEMORY_DDR_TYPE_DDR4) ||
            (result.chipName == "sl2619" && result.boardName == "coralboard" &&
             result.memoryDDRType == ASTRA_MEMORY_DDR_TYPE_DDR4X16));
    });
    return suite.Run(argc, argv);
}
