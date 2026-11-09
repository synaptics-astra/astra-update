// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include "flash_image_fixtures.hpp"

int main(int argc, char **argv)
{
    test::Suite suite;
    for (const bool emptyMaps : {false, true}) {
        suite.Add(emptyMaps ? "standalone-empty-maps" : "standalone-null-maps", [=] {
            test::TempDirectory directory;
            const auto path = directory.Write("nand.img", "payload");
            auto image = test::Nand(path, emptyMaps ? test::Maps({}) : nullptr);
            REQUIRE(image.Load() == 0 && image.GetLoadError().empty());
            REQUIRE(image.GetFlashCommand() == "usbload nand.img 0x10000000; m2nand 0x10000000");
            REQUIRE(image.GetFinalImage() == "nand.img");
            REQUIRE(image.GetFlashImageType() == FLASH_IMAGE_TYPE_NAND);
            REQUIRE(!image.GetResetWhenComplete());
            test::RequirePayloads(image, ASTRA_IMAGE_TYPE_UPDATE_NAND, {"nand.img"});
        });
    }
    for (const std::string type : {"config", "image"}) {
        suite.Add("manifest-default-file-" + type, [=] {
            test::TempDirectory directory;
            directory.Write("uNAND_full.img", "payload");
            auto image = test::Nand(directory.Path(), test::Maps({{{"type", type}}}));
            REQUIRE(image.Load() == 0);
            REQUIRE(image.GetFinalImage() == "uNAND_full.img");
            REQUIRE(image.GetFlashCommand() == "usbload uNAND_full.img 0x10000000; m2nand 0x10000000");
            test::RequirePayloads(image, ASTRA_IMAGE_TYPE_UPDATE_NAND, {"uNAND_full.img"});
        });
        suite.Add("manifest-custom-file-and-address-" + type, [=] {
            test::TempDirectory directory;
            directory.Write("custom.img", "payload");
            auto image = test::Nand(directory.Path(), test::Maps({
                {{"type", type}, {"image_file", "custom.img"}, {"read_address", "0x20000000"}}
            }));
            REQUIRE(image.Load() == 0);
            REQUIRE(image.GetFinalImage() == "custom.img");
            REQUIRE(image.GetFlashCommand() == "usbload custom.img 0x20000000; m2nand 0x20000000");
            test::RequirePayloads(image, ASTRA_IMAGE_TYPE_UPDATE_NAND, {"custom.img"});
        });
    }
    suite.Add("untyped-and-unrelated-maps-ignored", [] {
        test::TempDirectory directory;
        directory.Write("chosen.img", "payload");
        auto image = test::Nand(directory.Path(), test::Maps({
            {{"image_file", "not-selected.img"}}, {{"type", "other"}, {"image_file", "other.img"}},
            {{"type", "image"}, {"image_file", "chosen.img"}}
        }));
        REQUIRE(image.Load() == 0 && image.GetFinalImage() == "chosen.img");
    });
    suite.Add("first-recognized-map-selected", [] {
        test::TempDirectory directory;
        directory.Write("first.img", "first");
        auto image = test::Nand(directory.Path(), test::Maps({
            {{"type", "image"}, {"image_file", "first.img"}, {"read_address", "123"}},
            {{"type", "config"}, {"image_file", "missing.img"}, {"read_address", "456"}}
        }));
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetFlashCommand() == "usbload first.img 123; m2nand 123");
        test::RequirePayloads(image, ASTRA_IMAGE_TYPE_UPDATE_NAND, {"first.img"});
    });
    for (const std::string address : {"", "0", "123", "0XABCDEF", "$filesize"}) {
        suite.Add("valid-read-address-" + address, [=] {
            test::TempDirectory directory;
            directory.Write("nand.img", "payload");
            auto image = test::Nand(directory.Path(), test::Maps({
                {{"type", "config"}, {"image_file", "nand.img"}, {"read_address", address}}
            }));
            REQUIRE(image.Load() == 0);
            const auto expected = address.empty() ? "0x10000000" : address;
            REQUIRE(image.GetFlashCommand() == "usbload nand.img " + expected + "; m2nand " + expected);
        });
    }
    for (const std::string address : {"0x", "-1", " 1", "1;reset", "$other"}) {
        suite.Add("invalid-read-address-" + address, [=] {
            test::TempDirectory directory;
            directory.Write("nand.img", "payload");
            auto image = test::Nand(directory.Path(), test::Maps({
                {{"type", "config"}, {"image_file", "nand.img"}, {"read_address", address}}
            }));
            test::RequireLoadError(image, "Unsafe read_address in NAND manifest: '" + address + "'");
            REQUIRE(image.GetFlashCommand().empty());
        });
    }
    for (const std::string name : {"bad name.img", "bad;name.img", "nested/nand.img"}) {
        suite.Add("unsafe-image-file-" + name, [=] {
            test::TempDirectory directory;
            directory.Write(std::filesystem::path(name), "payload");
            auto image = test::Nand(directory.Path(), test::Maps({
                {{"type", "config"}, {"image_file", name}}
            }));
            test::RequireLoadError(image, "Unsafe image_file in NAND manifest:");
            REQUIRE(image.GetFlashCommand().empty());
        });
    }
    suite.Add("unsafe-standalone-name", [] {
        test::TempDirectory directory;
        auto image = test::Nand(directory.Write("bad name.img", "payload"));
        test::RequireLoadError(image, "Unsafe image_file in NAND manifest:");
    });
    suite.Add("missing-standalone-file", [] {
        test::TempDirectory directory;
        auto image = test::Nand(directory.Path() / "missing.img");
        test::RequireLoadError(image, "NAND image path does not exist:");
        REQUIRE(image.GetImages().empty());
    });
    suite.Add("missing-manifest-file", [] {
        test::TempDirectory directory;
        auto image = test::Nand(directory.Path(), test::Maps({
            {{"type", "config"}, {"image_file", "missing.img"}}
        }));
        test::RequireLoadError(image, "NAND image file not found:");
    });
    suite.Add("missing-default-manifest-file", [] {
        test::TempDirectory directory;
        auto image = test::Nand(directory.Path(), test::Maps({{{"type", "config"}}}));
        test::RequireLoadError(image, "uNAND_full.img");
    });
    suite.Add("unrecognized-maps-use-default-image", [] {
        test::TempDirectory directory;
        directory.Write("uNAND_full.img", "payload");
        auto image = test::Nand(directory.Path(), test::Maps({
            {}, {{"type", "other"}, {"image_file", "ignored"}}
        }));
        REQUIRE(image.Load() == 0 && image.GetFinalImage() == "uNAND_full.img");
    });
    suite.Add("empty-file-field-uses-default", [] {
        test::TempDirectory directory;
        directory.Write("uNAND_full.img", "payload");
        auto image = test::Nand(directory.Path(), test::Maps({
            {{"type", "config"}, {"image_file", ""}}
        }));
        REQUIRE(image.Load() == 0 && image.GetFinalImage() == "uNAND_full.img");
    });
    suite.Add("characterization-tag-applies-core-defaults-but-not-board", [] {
        test::TempDirectory directory;
        directory.Write("uNAND_full.img", "payload");
        directory.Write(test::TagName("sl2619", "coralboard"), "");
        auto image = test::Nand(directory.Path(), test::Maps({{{"type", "config"}}}), "");
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetChipName() == "sl2619");
        REQUIRE(image.GetSecureBootVersion() == ASTRA_SECURE_BOOT_V3);
        REQUIRE(image.GetMemoryLayout() == ASTRA_MEMORY_LAYOUT_2GB);
        REQUIRE(image.GetMemoryDDRType() == ASTRA_MEMORY_DDR_TYPE_DDR4X16);
        REQUIRE(image.GetBoardName() == "rdk");
    });
    suite.Add("explicit-chip-prevents-tag-overwrite", [] {
        test::TempDirectory directory;
        directory.Write("uNAND_full.img", "payload");
        directory.Write(test::TagName("sl1680"), "");
        auto image = test::Nand(directory.Path(), test::Maps({{{"type", "config"}}}));
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetChipName() == "sl2619");
        REQUIRE(image.GetSecureBootVersion() == ASTRA_SECURE_BOOT_V2);
        REQUIRE(image.GetMemoryLayout() == ASTRA_MEMORY_LAYOUT_1GB);
        REQUIRE(image.GetMemoryDDRType() == ASTRA_MEMORY_DDR_TYPE_DDR3);
    });
    suite.Add("standalone-no-tag-leaves-chip-unspecified", [] {
        test::TempDirectory directory;
        auto image = test::Nand(directory.Write("nand.img", "payload"), nullptr, "");
        REQUIRE(image.Load() == 0 && image.GetChipName().empty());
    });
    suite.Add("characterization-repeated-load-appends", [] {
        test::TempDirectory directory;
        auto image = test::Nand(directory.Write("nand.img", "payload"));
        REQUIRE(image.Load() == 0);
        const auto command = image.GetFlashCommand();
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetImages().size() == 2 && image.GetFlashCommand() == command);
    });
    suite.Add("version-change-retains-nand-command", [] {
        test::TempDirectory directory;
        auto image = test::Nand(directory.Write("nand.img", "payload"));
        REQUIRE(image.Load() == 0);
        const auto command = image.GetFlashCommand();
        for (const auto version : {ASTRA_UBOOT_VERSION_2019_10, ASTRA_UBOOT_VERSION_2025_01}) {
            image.SetUbootVersion(version);
            REQUIRE(image.GetUbootVersion() == version);
            REQUIRE(image.GetFlashCommand() == command);
            REQUIRE(image.GetImages().size() == 1);
        }
    });
    suite.Add("factory-image-map-fields-reach-nand-command", [] {
        test::TempDirectory directory;
        directory.Write("nand.img", "payload");
        directory.Write("manifest.yaml", "image_type: nand\nimages:\n  nand.img:\n    read_address: '456'\n");
        test::Manifest config;
        auto image = FlashImage::FlashImageFactory(directory.Path().string(), config);
        REQUIRE(image->Load() == 0);
        REQUIRE(image->GetFlashCommand() == "usbload nand.img 456; m2nand 456");
    });
    return suite.Run(argc, argv);
}
