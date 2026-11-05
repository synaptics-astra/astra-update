// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include "flash_image_fixtures.hpp"
#include "unreadable_file_fixture.hpp"

int main(int argc, char **argv)
{
    test::Suite suite;
    suite.Add("discovers-images-and-final-flashable-entry", [] {
        test::TempDirectory directory;
        directory.Write("emmc_part_list", "partitions");
        directory.Write("emmc_image_list", "subimg-first.bin,0\nsubimg-last.bin,1\nerase,2\nformat,3\n");
        directory.Write("subimg-first.bin", "first");
        directory.Write("subimg-last.bin", "last");
        directory.Write("other.txt", "ignored");
        directory.Write("manifest.yaml", "ignored");
        auto image = test::Emmc(directory.Path());
        REQUIRE(image.Load() == 0 && image.GetLoadError().empty());
        REQUIRE(image.GetFlashImageType() == FLASH_IMAGE_TYPE_EMMC);
        REQUIRE(image.GetFlashCommand() == "l2emmc " + directory.Path().filename().string());
        REQUIRE(image.GetFinalImage() == "subimg-last.bin");
        REQUIRE(!image.GetResetWhenComplete());
        test::RequirePayloads(image, ASTRA_IMAGE_TYPE_UPDATE_EMMC,
            {"emmc_part_list", "emmc_image_list", "subimg-first.bin", "subimg-last.bin"}, false);
    });
    const std::vector<std::pair<std::string, std::string>> lists = {
        {"subimg.bin,0,1\n", "subimg.bin"},
        {"erase,0\nsubimg.bin,0\nformat,0\n", "subimg.bin"},
        {"\nsubimg.bin,0\n\n", "subimg.bin"},
        {"subimg.bin", "subimg.bin"},
        {"subimg.bin,0\r\nerase,0\r\nformat,0\r\n", "subimg.bin"},
        {"subimg.bin,0", "subimg.bin"}
    };
    suite.Add("trailing-slash-directory", [] {
        test::TempDirectory directory;
        directory.Write("emmc_image_list", "subimg.bin,0\n");
        directory.Write("subimg.bin", "payload");
        auto image = test::Emmc(std::filesystem::path(directory.Path().string() + "/"));
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetFlashCommand() == "l2emmc " + directory.Path().filename().string());
        REQUIRE(image.GetFinalImage() == "subimg.bin");
    });
    suite.Add("version-change-retains-emmc-command", [] {
        test::TempDirectory directory;
        directory.Write("emmc_image_list", "subimg.bin,0\n");
        directory.Write("subimg.bin", "payload");
        auto image = test::Emmc(directory.Path());
        REQUIRE(image.Load() == 0);
        const auto command = image.GetFlashCommand();
        for (const auto version : {ASTRA_UBOOT_VERSION_2019_10, ASTRA_UBOOT_VERSION_2025_01}) {
            image.SetUbootVersion(version);
            REQUIRE(image.GetUbootVersion() == version);
            REQUIRE(image.GetFlashCommand() == command);
            REQUIRE(image.GetImages().size() == 2);
        }
    });
    for (size_t i = 0; i < lists.size(); ++i) {
        suite.Add("image-list-format-" + std::to_string(i), [entry = lists[i]] {
            test::TempDirectory directory;
            directory.Write("emmc_image_list", entry.first);
            directory.Write("subimg.bin", "payload");
            auto image = test::Emmc(directory.Path());
            REQUIRE(image.Load() == 0);
            REQUIRE(image.GetFinalImage() == entry.second);
        });
    }
    const std::vector<std::string> emptyLists = {"", "\n", "erase,0\nformat,0\n", ",\n"};
    for (size_t i = 0; i < emptyLists.size(); ++i) {
        suite.Add("non-flashable-image-list-" + std::to_string(i), [list = emptyLists[i]] {
            test::TempDirectory directory;
            directory.Write("emmc_image_list", list);
            auto image = test::Emmc(directory.Path());
            test::RequireLoadError(image, "No flashable entries in");
            REQUIRE(image.GetFinalImage().empty());
        });
    }
    suite.Add("missing-directory", [] {
        test::TempDirectory directory;
        auto image = test::Emmc(directory.Path() / "missing");
        test::RequireLoadError(image, "eMMC image directory not found:");
    });
    suite.Add("regular-file-not-image-directory", [] {
        test::TempDirectory directory;
        auto image = test::Emmc(directory.Write("file.bin", "data"));
        test::RequireLoadError(image, "eMMC image directory not found:");
    });
    suite.Add("no-emmc-images", [] {
        test::TempDirectory directory;
        directory.Write("ordinary.bin", "ignored");
        auto image = test::Emmc(directory.Path());
        test::RequireLoadError(image, "No eMMC images found in");
    });
    suite.Add("missing-image-list", [] {
        test::TempDirectory directory;
        directory.Write("subimg.bin", "payload");
        auto image = test::Emmc(directory.Path());
        test::RequireLoadError(image, "emmc_image_list not found in");
    });
    suite.Add("unreadable-image-list", [] {
        test::TempDirectory directory;
        const auto list = directory.Write("emmc_image_list", "subimg.bin,0\n");
        test::UnreadableFile unreadable(list);
        unreadable.RequireUnreadable();
        auto image = test::Emmc(directory.Path());
        test::RequireLoadError(image, "Failed to open");
    });
    for (const std::string name : {"bad name", "bad;name"}) {
        suite.Add("unsafe-directory-" + name, [=] {
            test::TempDirectory directory;
            const auto path = directory.Path() / name;
            std::filesystem::create_directory(path);
            auto image = test::Emmc(path);
            test::RequireLoadError(image, "Image directory name cannot be used in a U-Boot command:");
            REQUIRE(image.GetFlashCommand().empty());
        });
    }
    suite.Add("characterization-tag-applies-core-defaults-but-not-board", [] {
        test::TempDirectory directory;
        directory.Write("emmc_image_list", "subimg.bin,0\n");
        directory.Write("subimg.bin", "payload");
        directory.Write(test::TagName("sl1680", "coralboard"), "");
        auto image = test::Emmc(directory.Path(), "");
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetChipName() == "sl1680");
        REQUIRE(image.GetSecureBootVersion() == ASTRA_SECURE_BOOT_V3);
        REQUIRE(image.GetMemoryLayout() == ASTRA_MEMORY_LAYOUT_4GB);
        REQUIRE(image.GetMemoryDDRType() == ASTRA_MEMORY_DDR_TYPE_LPDDR4X);
        REQUIRE(image.GetBoardName() == "rdk");
    });
    suite.Add("explicit-chip-and-memory-not-overwritten-by-tag", [] {
        test::TempDirectory directory;
        directory.Write("emmc_image_list", "subimg.bin,0\n");
        directory.Write("subimg.bin", "payload");
        directory.Write(test::TagName("sl1680"), "");
        auto image = test::Emmc(directory.Path(), "sl2619");
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetChipName() == "sl2619");
        REQUIRE(image.GetSecureBootVersion() == ASTRA_SECURE_BOOT_V2);
        REQUIRE(image.GetMemoryLayout() == ASTRA_MEMORY_LAYOUT_1GB);
        REQUIRE(image.GetMemoryDDRType() == ASTRA_MEMORY_DDR_TYPE_DDR3);
    });
    suite.Add("no-tag-leaves-unspecified-chip", [] {
        test::TempDirectory directory;
        directory.Write("emmc_image_list", "subimg.bin,0\n");
        directory.Write("subimg.bin", "payload");
        auto image = test::Emmc(directory.Path(), "");
        REQUIRE(image.Load() == 0 && image.GetChipName().empty());
    });
    suite.Add("characterization-missing-payload-accepted-at-list-parse", [] {
        test::TempDirectory directory;
        directory.Write("emmc_image_list", "missing-subimg.bin,0\n");
        auto image = test::Emmc(directory.Path());
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetFinalImage() == "missing-subimg.bin");
        REQUIRE(image.GetImages().size() == 1);
    });
    suite.Add("characterization-repeated-load-appends", [] {
        test::TempDirectory directory;
        directory.Write("emmc_image_list", "subimg.bin,0\n");
        directory.Write("subimg.bin", "payload");
        auto image = test::Emmc(directory.Path());
        REQUIRE(image.Load() == 0);
        const auto command = image.GetFlashCommand();
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetImages().size() == 4);
        REQUIRE(image.GetFlashCommand() == command);
    });
    suite.Add("characterization-list-name-whitespace-preserved", [] {
        test::TempDirectory directory;
        directory.Write("emmc_image_list", " subimg.bin ,0\n");
        auto image = test::Emmc(directory.Path());
        REQUIRE(image.Load() == 0 && image.GetFinalImage() == " subimg.bin ");
    });
    suite.Add("characterization-comma-only-last-line-clears-final-image", [] {
        test::TempDirectory directory;
        directory.Write("emmc_image_list", "subimg.bin,0\n,\n");
        auto image = test::Emmc(directory.Path());
        test::RequireLoadError(image, "No flashable entries in");
    });
    return suite.Run(argc, argv);
}
