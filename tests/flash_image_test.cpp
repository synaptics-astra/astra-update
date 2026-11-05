// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include "flash_image_fixtures.hpp"
#include "unreadable_file_fixture.hpp"

namespace {

std::shared_ptr<FlashImage> Factory(const test::TempDirectory &directory, test::Manifest config,
    const std::string &manifest = "")
{
    const auto original = config;
    auto image = FlashImage::FlashImageFactory(directory.Path().string(), config, manifest);
    REQUIRE(config == original);
    REQUIRE(image != nullptr);
    return image;
}

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    for (const std::string type : {"spi", "nand", "emmc"}) {
        suite.Add("explicit-type-" + type, [=] {
            test::TempDirectory directory;
            auto image = Factory(directory, {{"image_type", type}});
            REQUIRE(image->GetFlashImageType() == (type == "spi" ? FLASH_IMAGE_TYPE_SPI :
                type == "nand" ? FLASH_IMAGE_TYPE_NAND : FLASH_IMAGE_TYPE_EMMC));
            REQUIRE(image->GetImages().empty());
            REQUIRE(image->GetFlashCommand().empty() && image->GetLoadError().empty());
            REQUIRE(image->GetUbootVersion() == ASTRA_UBOOT_VERSION_UNKNOWN);
            REQUIRE(image->GetResetWhenComplete());
            REQUIRE(image->GetSecureBootVersion() == ASTRA_SECURE_BOOT_V3);
            REQUIRE(image->GetMemoryLayout() == ASTRA_MEMORY_LAYOUT_2GB);
            REQUIRE(image->GetMemoryDDRType() == ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED);
            if (type == "spi") {
                REQUIRE(std::dynamic_pointer_cast<SpiFlashImage>(image) != nullptr);
            } else if (type == "nand") {
                REQUIRE(std::dynamic_pointer_cast<NandFlashImage>(image) != nullptr);
            } else {
                REQUIRE(std::dynamic_pointer_cast<EmmcFlashImage>(image) != nullptr);
            }
        });
    }
    for (const std::string type : {"SPI", "", "unknown", "config"}) {
        suite.Add("invalid-type-" + type, [=] {
            test::TempDirectory directory;
            REQUIRE_THROWS(std::invalid_argument, Factory(directory, {{"image_type", type}}));
        });
    }
    suite.Add("unknown-type-without-detection", [] {
        test::TempDirectory directory;
        REQUIRE_THROWS(std::invalid_argument, Factory(directory, {}));
    });
    suite.Add("missing-image-path", [] {
        test::TempDirectory directory;
        test::Manifest config{{"image_type", "spi"}};
        REQUIRE_THROWS(std::invalid_argument,
            FlashImage::FlashImageFactory((directory.Path() / "missing").string(), config));
    });
    suite.Add("standalone-file-needs-explicit-type", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("boot.bin", "payload");
        test::Manifest config;
        REQUIRE_THROWS(std::invalid_argument, FlashImage::FlashImageFactory(path.string(), config));
        config["image_type"] = "spi";
        auto image = FlashImage::FlashImageFactory(path.string(), config);
        REQUIRE(image->Load() == 0);
        test::RequirePayloads(*image, ASTRA_IMAGE_TYPE_UPDATE_SPI, {"boot.bin"});
    });
    suite.Add("manifest-values-normalized-and-applied", [] {
        test::TempDirectory directory;
        directory.Write("manifest.yaml", test::ManifestYaml({
            {"image_type", "spi"}, {"boot_image", "Exact-ID"}, {"chip", "SL1680"}, {"board", "RDK"},
            {"secure_boot", "GEN2"}, {"memory_layout", "512MB"}, {"ddr_type", "LPDDR4X"},
            {"reset", "disable"}, {"image_file", "boot.bin"}
        }));
        directory.Write("boot.bin", "payload");
        auto image = Factory(directory, {});
        REQUIRE(image->GetBootImageId() == "Exact-ID");
        REQUIRE(image->GetChipName() == "sl1680" && image->GetBoardName() == "rdk");
        REQUIRE(image->GetSecureBootVersion() == ASTRA_SECURE_BOOT_V2);
        REQUIRE(image->GetMemoryLayout() == ASTRA_MEMORY_LAYOUT_512MB);
        REQUIRE(image->GetMemoryDDRType() == ASTRA_MEMORY_DDR_TYPE_LPDDR4X);
        REQUIRE(!image->GetResetWhenComplete());
        REQUIRE(image->Load() == 0);
        test::RequirePayloads(*image, ASTRA_IMAGE_TYPE_UPDATE_SPI, {"boot.bin"});
    });
    suite.Add("command-line-fields-override-manifest", [] {
        test::TempDirectory directory;
        directory.Write("manifest.yaml", test::ManifestYaml({
            {"image_type", "nand"}, {"boot_image", "manifest"}, {"chip", "sl1680"}, {"board", "coralboard"},
            {"secure_boot", "gen2"}, {"memory_layout", "4gb"}, {"ddr_type", "ddr3"},
            {"reset", "disable"}, {"image_file", "manifest.img"}
        }));
        directory.Write("cli.img", "payload");
        auto image = Factory(directory, {
            {"image_type", "spi"}, {"boot_image", "cli"}, {"chip", "SL2619"}, {"board", "RDK"},
            {"secure_boot", "GENX"}, {"memory_layout", "1GB"}, {"ddr_type", "DDR4"},
            {"reset", "enable"}, {"image_file", "cli.img"}
        });
        REQUIRE(image->GetFlashImageType() == FLASH_IMAGE_TYPE_SPI);
        REQUIRE(image->GetBootImageId() == "cli");
        REQUIRE(image->GetChipName() == "sl2619" && image->GetBoardName() == "rdk");
        REQUIRE(image->GetSecureBootVersion() == ASTRA_SECURE_BOOT_V3);
        REQUIRE(image->GetMemoryLayout() == ASTRA_MEMORY_LAYOUT_1GB);
        REQUIRE(image->GetMemoryDDRType() == ASTRA_MEMORY_DDR_TYPE_DDR4);
        REQUIRE(image->GetResetWhenComplete());
        REQUIRE(image->Load() == 0);
        test::RequirePayloads(*image, ASTRA_IMAGE_TYPE_UPDATE_SPI, {"cli.img"});
    });
    suite.Add("explicit-empty-command-line-value-wins", [] {
        test::TempDirectory directory;
        directory.Write("manifest.yaml", test::ManifestYaml({
            {"image_type", "spi"}, {"chip", "sl1680"}, {"board", "coralboard"}, {"boot_image", "manifest"}
        }));
        const auto image = Factory(directory, {{"chip", ""}, {"board", ""}, {"boot_image", ""}});
        REQUIRE(image->GetChipName().empty() && image->GetBoardName().empty());
        REQUIRE(image->GetBootImageId().empty());
        REQUIRE(image->GetMemoryLayout() == ASTRA_MEMORY_LAYOUT_2GB);
        REQUIRE(image->GetMemoryDDRType() == ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED);
    });
    suite.Add("custom-manifest-path", [] {
        test::TempDirectory directory;
        directory.Write("manifest.yaml", test::ManifestYaml({{"image_type", "nand"}, {"boot_image", "wrong"}}));
        const auto custom = directory.Write("custom.yaml", test::ManifestYaml({
            {"image_type", "spi"}, {"boot_image", "custom"}, {"image_file", "boot.bin"}
        }));
        directory.Write("boot.bin", "payload");
        auto image = Factory(directory, {}, custom.string());
        REQUIRE(image->GetBootImageId() == "custom" && image->GetFlashImageType() == FLASH_IMAGE_TYPE_SPI);
        REQUIRE(image->Load() == 0);
    });
    suite.Add("missing-manifest-logs-and-uses-config", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("standalone.bin", "payload");
        test::Manifest config{{"image_type", "spi"}, {"chip", "SL2619"}};
        const auto logPath = directory.Path() / "load.log";
        {
            test::LogSession log(logPath.string(), ASTRA_LOG_LEVEL_DEBUG);
            auto image = FlashImage::FlashImageFactory(path.string(), config,
                (directory.Path() / "missing.yaml").string());
            REQUIRE(image->GetChipName() == "sl2619");
            REQUIRE(image->Load() == 0);
        }
        REQUIRE(test::ReadFile(logPath).find("No manifest file found at:") != std::string::npos);
    });
    suite.Add("unreadable-manifest-falls-back-to-config", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("standalone.bin", "payload");
        const auto manifest = directory.Write("manifest.yaml", "image_type: nand\n");
        test::UnreadableFile unreadable(manifest);
        unreadable.RequireUnreadable();
        test::Manifest config{{"image_type", "spi"}, {"chip", "sl2619"}};
        const auto original = config;
        const auto logPath = directory.Path() / "load.log";
        {
            test::LogSession log(logPath.string(), ASTRA_LOG_LEVEL_DEBUG);
            auto image = FlashImage::FlashImageFactory(path.string(), config, manifest.string());
            REQUIRE(config == original);
            REQUIRE(image->GetFlashImageType() == FLASH_IMAGE_TYPE_SPI);
            REQUIRE(image->GetChipName() == "sl2619");
            REQUIRE(image->Load() == 0);
            test::RequirePayloads(*image, ASTRA_IMAGE_TYPE_UPDATE_SPI, {"standalone.bin"});
        }
        REQUIRE(test::ReadFile(logPath).find("No manifest file found at:") != std::string::npos);
    });
    for (const auto layout : test::layouts) {
        suite.Add(std::string("configured-layout-") + layout.name, [=] {
            test::TempDirectory directory;
            REQUIRE(Factory(directory, {{"image_type", "spi"}, {"memory_layout", layout.name}})->GetMemoryLayout() == layout.value);
        });
    }
    for (const auto ddr : test::ddrTypes) {
        suite.Add(std::string("configured-ddr-") + ddr.name, [=] {
            test::TempDirectory directory;
            REQUIRE(Factory(directory, {{"image_type", "spi"}, {"ddr_type", ddr.name}})->GetMemoryDDRType() == ddr.value);
        });
    }
    suite.Add("invalid-layout-rejected", [] {
        test::TempDirectory directory;
        REQUIRE_THROWS(std::invalid_argument, Factory(directory, {{"image_type", "spi"}, {"memory_layout", "8gb"}}));
    });
    for (const std::string chip : {"sl1680", "sl1640", "sl1620", "sl2610", "sl2619", ""}) {
        suite.Add("chip-memory-default-" + chip, [=] {
            test::TempDirectory directory;
            const auto image = Factory(directory, {{"image_type", "spi"}, {"chip", chip}});
            REQUIRE(image->GetMemoryLayout() == (chip == "sl1680" ? ASTRA_MEMORY_LAYOUT_4GB : ASTRA_MEMORY_LAYOUT_2GB));
        });
    }
    for (const std::string ddr : {"", "not_specified", "unknown", "ddr3"}) {
        suite.Add("coralboard-ddr-default-" + ddr, [=] {
            test::TempDirectory directory;
            const auto image = Factory(directory, {{"image_type", "spi"}, {"board", "CORALBOARD"}, {"ddr_type", ddr}});
            REQUIRE(image->GetBoardName() == "coralboard");
            REQUIRE(image->GetMemoryDDRType() ==
                (ddr == "ddr3" ? ASTRA_MEMORY_DDR_TYPE_DDR3 : ASTRA_MEMORY_DDR_TYPE_DDR4X16));
        });
    }
    suite.Add("characterization-invalid-ddr-and-secure-boot-default", [] {
        test::TempDirectory directory;
        const auto image = Factory(directory, {
            {"image_type", "spi"}, {"ddr_type", "unknown"}, {"secure_boot", "unknown"}
        });
        REQUIRE(image->GetSecureBootVersion() == ASTRA_SECURE_BOOT_V3);
        REQUIRE(image->GetMemoryDDRType() == ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED);
    });
    for (const std::string reset : {"enable", "disable", "", "ENABLE", "unknown"}) {
        suite.Add("reset-value-" + reset, [=] {
            test::TempDirectory directory;
            REQUIRE(Factory(directory, {{"image_type", "spi"}, {"reset", reset}})->GetResetWhenComplete() == (reset == "enable"));
        });
    }
    suite.Add("detects-emmc-partition-list", [] {
        test::TempDirectory directory;
        directory.Write("emmc_part_list", "");
        REQUIRE(Factory(directory, {})->GetFlashImageType() == FLASH_IMAGE_TYPE_EMMC);
    });
    suite.Add("nand-tag-preferred-over-emmc-detection", [] {
        test::TempDirectory directory;
        directory.Write("TAG--astra-media-sl2619-rdk-nand.rootfs", "");
        directory.Write("emmc_part_list", "");
        REQUIRE(Factory(directory, {})->GetFlashImageType() == FLASH_IMAGE_TYPE_NAND);
        REQUIRE(Factory(directory, {{"image_type", "spi"}})->GetFlashImageType() == FLASH_IMAGE_TYPE_SPI);
    });
    for (const std::string name : {"TAG--other-nand", "astra-nand", "TAG--astra-emmc"}) {
        suite.Add("incomplete-nand-marker-" + name, [=] {
            test::TempDirectory directory;
            directory.Write(name, "");
            REQUIRE_THROWS(std::invalid_argument, Factory(directory, {}));
        });
    }
    suite.Add("multi-image-manifest-preserves-yaml-order", [] {
        test::TempDirectory directory;
        directory.Write("manifest.yaml",
            "image_type: spi\nimages:\n  z.bin:\n    read_address: '0x1234'\n  a.bin: {}\n");
        directory.Write("z.bin", "z");
        directory.Write("a.bin", "a");
        auto image = Factory(directory, {});
        REQUIRE(image->Load() == 0);
        test::RequirePayloads(*image, ASTRA_IMAGE_TYPE_UPDATE_SPI, {"z.bin", "a.bin"});
        REQUIRE(image->GetFinalImage() == "a.bin");
        REQUIRE(image->GetFlashCommand().find("usbload z.bin 0x1234;") == 0);
    });
    suite.Add("image-properties-reach-spi-command", [] {
        test::TempDirectory directory;
        directory.Write("manifest.yaml", "image_type: spi\nimages:\n  boot.bin:\n"
            "    read_address: '123'\n    write_length: '$filesize'\n");
        directory.Write("boot.bin", "payload");
        auto image = Factory(directory, {});
        REQUIRE(image->Load() == 0);
        REQUIRE(image->GetFlashCommand() ==
            "usbload boot.bin 123; sf probe; sf erase 0 0x200000; sf write 123 0 $filesize; "
            "sf erase 0x200000 0x200000; sf write 123 0x200000 $filesize; ");
    });
    suite.Add("all-image-properties-reach-command-through-factory", [] {
        test::TempDirectory directory;
        directory.Write("boot.bin", "payload");
        directory.Write("manifest.yaml", "image_type: spi\nimages:\n  boot.bin:\n"
            "    read_address: '0x11'\n    write_first_copy_address: '0x22'\n"
            "    write_second_copy_address: '0x33'\n    write_length: '0x44'\n"
            "    erase_first_start_address: '0x55'\n    erase_first_length: '0x66'\n"
            "    erase_second_start_address: '0x77'\n    erase_second_length: '0x88'\n");
        auto image = Factory(directory, {});
        REQUIRE(image->Load() == 0);
        REQUIRE(image->GetFlashCommand() ==
            "usbload boot.bin 0x11; sf probe; sf erase 0x55 0x66; sf write 0x11 0x22 0x44; "
            "sf erase 0x77 0x88; sf write 0x11 0x33 0x44; ");
    });
    for (const std::string properties : {"", "null", "ignored-scalar"}) {
        suite.Add("image-entry-without-properties-" + properties, [=] {
            test::TempDirectory directory;
            directory.Write("boot.bin", "payload");
            directory.Write("manifest.yaml", "image_type: spi\nimages:\n  boot.bin: " + properties + "\n");
            auto image = Factory(directory, {});
            REQUIRE(image->Load() == 0);
            test::RequirePayloads(*image, ASTRA_IMAGE_TYPE_UPDATE_SPI, {"boot.bin"});
            REQUIRE(image->GetFinalImage() == "boot.bin");
        });
    }
    suite.Add("characterization-global-address-not-applied-to-image-map", [] {
        test::TempDirectory directory;
        directory.Write("boot.bin", "payload");
        directory.Write("manifest.yaml", "image_type: spi\nimages:\n  boot.bin: {}\n");
        auto image = Factory(directory, {{"read_address", "123"}});
        REQUIRE(image->Load() == 0);
        REQUIRE(image->GetFlashCommand() ==
            "usbload boot.bin 0x10000000; sf probe; sf erase 0 0x200000; sf write 0x10000000 0 0x200000; "
            "sf erase 0x200000 0x200000; sf write 0x10000000 0x200000 0x200000; ");
    });
    suite.Add("characterization-missing-manifest-drops-image-file-config", [] {
        test::TempDirectory directory;
        directory.Write("boot.bin", "payload");
        auto image = Factory(directory, {{"image_type", "spi"}, {"image_file", "boot.bin"}});
        REQUIRE(image->Load() == 0);
        REQUIRE(image->GetFinalImage() == directory.Path().filename().string());
        REQUIRE(image->GetImages().size() == 1);
        auto payload = image->GetImages().front();
        REQUIRE(payload.Load() == -1);
    });
    const std::vector<std::string> invalidYaml = {
        "[", "images: [boot.bin]", "images:\n  boot.bin:\n    read_address: [1, 2]"
    };
    suite.Add("characterization-invalid-manifest-keeps-earlier-images", [] {
        test::TempDirectory directory;
        directory.Write("first.bin", "payload");
        directory.Write("manifest.yaml", "images:\n  first.bin: {}\n"
            "  invalid.bin:\n    read_address: [1, 2]\n");
        const auto logPath = directory.Path() / "warnings.log";
        {
            test::LogSession log(logPath.string(), ASTRA_LOG_LEVEL_WARNING);
            auto image = Factory(directory, {{"image_type", "spi"}});
            REQUIRE(image->Load() == 0);
            REQUIRE(image->GetFinalImage() == "first.bin");
            test::RequirePayloads(*image, ASTRA_IMAGE_TYPE_UPDATE_SPI, {"first.bin"});
            REQUIRE(image->GetFlashCommand() ==
                "usbload first.bin 0x10000000; sf probe; sf erase 0 0x200000; sf write 0x10000000 0 0x200000; "
                "sf erase 0x200000 0x200000; sf write 0x10000000 0x200000 0x200000; ");
        }
        REQUIRE(test::ReadFile(logPath).find("Invalid manifest file:") != std::string::npos);
    });
    for (size_t i = 0; i < invalidYaml.size(); ++i) {
        suite.Add("characterization-invalid-manifest-warning-" + std::to_string(i), [yaml = invalidYaml[i]] {
            test::TempDirectory directory;
            const auto manifest = directory.Write("invalid.yaml", yaml);
            const auto imagePath = directory.Write("boot.bin", "payload");
            const auto logPath = directory.Path() / "warnings.log";
            {
                test::LogSession log(logPath.string(), ASTRA_LOG_LEVEL_WARNING);
                test::Manifest config{{"image_type", "spi"}};
                auto image = FlashImage::FlashImageFactory(imagePath.string(), config, manifest.string());
                REQUIRE(image->Load() == 0);
            }
            REQUIRE(test::ReadFile(logPath).find("Invalid manifest file:") != std::string::npos);
        });
    }
    for (const std::string yaml : {"scalar", "[]", "{}"}) {
        suite.Add("characterization-empty-or-nonmap-manifest-" + yaml, [=] {
            test::TempDirectory directory;
            directory.Write("manifest.yaml", yaml);
            auto image = Factory(directory, {{"image_type", "spi"}});
            test::RequireLoadError(*image, "No SPI images configured");
        });
    }
    suite.Add("empty-images-load-failure", [] {
        test::TempDirectory directory;
        directory.Write("manifest.yaml", "image_type: spi\nimages: {}\n");
        auto image = Factory(directory, {});
        test::RequireLoadError(*image, "No SPI images configured");
    });
    suite.Add("default-emmcimg-path-and-missing-defaults", [] {
        test::TempDirectory directory;
        test::WorkingDirectory cwd(directory.Path());
        test::Manifest config;
        REQUIRE_THROWS(std::invalid_argument, FlashImage::FlashImageFactory("eMMCimg", config));
        directory.Write(std::filesystem::path("eMMCimg") / "emmc_part_list", "");
        directory.Write(std::filesystem::path("eMMCimg") / "manifest.yaml",
            test::ManifestYaml({{"boot_image", "default-image"}}));
        const auto image = FlashImage::FlashImageFactory("eMMCimg", config);
        REQUIRE(image->GetBootImageId() == "default-image");
        REQUIRE(image->GetFlashImageType() == FLASH_IMAGE_TYPE_EMMC);
    });
    suite.Add("characterization-synaimg-fallback-does-not-read-its-manifest", [] {
        test::TempDirectory directory;
        test::WorkingDirectory cwd(directory.Path());
        directory.Write(std::filesystem::path("SYNAIMG") / "emmc_part_list", "");
        directory.Write(std::filesystem::path("SYNAIMG") / "emmc_image_list", "subimg.bin,0\n");
        directory.Write(std::filesystem::path("SYNAIMG") / "subimg.bin", "payload");
        directory.Write(std::filesystem::path("SYNAIMG") / "manifest.yaml",
            test::ManifestYaml({{"boot_image", "fallback-manifest"}}));
        test::Manifest config;
        auto image = FlashImage::FlashImageFactory("eMMCimg", config);
        REQUIRE(image->GetBootImageId().empty());
        REQUIRE(image->GetFlashImageType() == FLASH_IMAGE_TYPE_EMMC);
        REQUIRE(image->Load() == 0 && image->GetFlashCommand() == "l2emmc SYNAIMG");
    });
    suite.Add("synaimg-fallback-with-explicit-manifest", [] {
        test::TempDirectory directory;
        test::WorkingDirectory cwd(directory.Path());
        const auto manifest = directory.Write(std::filesystem::path("SYNAIMG") / "manifest.yaml",
            test::ManifestYaml({{"image_type", "emmc"}, {"boot_image", "explicit"}}));
        test::Manifest config;
        const auto image = FlashImage::FlashImageFactory("eMMCimg", config, manifest.string());
        REQUIRE(image->GetBootImageId() == "explicit");
    });
    return suite.Run(argc, argv);
}
