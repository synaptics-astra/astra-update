// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include "flash_image_fixtures.hpp"

namespace {

const std::string modernFixed =
    "usbload boot.bin 0x10000000; sf probe; sf erase 0 0x200000; sf write 0x10000000 0 0x200000; "
    "sf erase 0x200000 0x200000; sf write 0x10000000 0x200000 0x200000; ";
const std::string modernFileSize =
    "usbload boot.bin 0x10000000; sf probe; sf erase 0 0x200000; sf write 0x10000000 0 $filesize; "
    "sf erase 0x200000 0x200000; sf write 0x10000000 0x200000 $filesize; ";
const std::string legacyFixed =
    "usbload boot.bin 0x10000000; spinit; erase 0xf0000000 0xf01fffff; cp.b 0x10000000 0xf0000000 0x200000; "
    "erase 0xf0200000 0xf03fffff; cp.b 0x10000000 0xf0200000 0x200000; ";
const std::string legacyFileSize =
    "usbload boot.bin 0x10000000; spinit; erase 0 0x200000; cp.b 0x10000000 0 $filesize; "
    "erase 0x200000 0x200000; cp.b 0x10000000 0x200000 $filesize; ";

constexpr const char *addressKeys[] = {
    "read_address", "write_first_copy_address", "write_second_copy_address", "write_length",
    "erase_first_start_address", "erase_first_length", "erase_second_start_address", "erase_second_length"
};

std::string RenameCommand(std::string command, const std::string &name)
{
    command.replace(8, std::string("boot.bin").size(), name);
    return command;
}

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    for (const std::string chip : {"sl1620", "sl1640", "sl1680", "sl2610", "sl2619"}) {
        for (const auto version : {ASTRA_UBOOT_VERSION_UNKNOWN, ASTRA_UBOOT_VERSION_2019_10,
            ASTRA_UBOOT_VERSION_2025_01}) {
            suite.Add("default-command-" + chip + "-" + std::to_string(version), [=] {
                test::TempDirectory directory;
                const auto path = directory.Write("boot.bin", "payload");
                auto image = test::Spi(path, nullptr, chip, version);
                REQUIRE(image.Load() == 0 && image.GetLoadError().empty());
                const bool sl261x = chip == "sl2610" || chip == "sl2619";
                const auto expected = version == ASTRA_UBOOT_VERSION_2019_10 ?
                    (sl261x ? legacyFileSize : legacyFixed) : (sl261x ? modernFileSize : modernFixed);
                REQUIRE(image.GetFlashCommand() == expected);
                REQUIRE(image.GetFinalImage() == "boot.bin");
                REQUIRE(image.GetFlashImageType() == FLASH_IMAGE_TYPE_SPI && image.GetResetWhenComplete());
                test::RequirePayloads(image, ASTRA_IMAGE_TYPE_UPDATE_SPI, {"boot.bin"});
            });
        }
    }
    for (const std::string type : {"config", "image"}) {
        suite.Add("single-image-manifest-" + type, [=] {
            test::TempDirectory directory;
            directory.Write("boot.bin", "payload");
            auto image = test::Spi(directory.Path(), test::Maps({
                {{"type", type}, {"image_file", "boot.bin"}}
            }));
            REQUIRE(image.Load() == 0);
            REQUIRE(image.GetFlashCommand() == modernFixed && image.GetFinalImage() == "boot.bin");
            test::RequirePayloads(image, ASTRA_IMAGE_TYPE_UPDATE_SPI, {"boot.bin"});
        });
    }
    suite.Add("standalone-empty-maps", [] {
        test::TempDirectory directory;
        auto image = test::Spi(directory.Write("boot.bin", "payload"), test::Maps({}));
        REQUIRE(image.Load() == 0 && image.GetFlashCommand() == modernFixed);
    });
    suite.Add("multiple-images-ordered-and-final-image", [] {
        test::TempDirectory directory;
        directory.Write("first.bin", "first");
        directory.Write("last.bin", "last");
        auto image = test::Spi(directory.Path(), test::Maps({
            {{"type", "image"}, {"image_file", "first.bin"}},
            {{"type", "image"}, {"image_file", "last.bin"}},
            {{"type", "config"}, {"chip", "sl1680"}}
        }));
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetFlashCommand() == RenameCommand(modernFixed, "first.bin") + RenameCommand(modernFixed, "last.bin"));
        REQUIRE(image.GetFinalImage() == "last.bin");
        test::RequirePayloads(image, ASTRA_IMAGE_TYPE_UPDATE_SPI, {"first.bin", "last.bin"});
    });
    for (const auto version : {ASTRA_UBOOT_VERSION_2019_10, ASTRA_UBOOT_VERSION_2025_01}) {
        for (const std::string chip : {"sl1680", "sl2619"}) {
            suite.Add("all-address-overrides-" + chip + "-" + std::to_string(version), [=] {
                test::TempDirectory directory;
                directory.Write("boot.bin", "payload");
                auto image = test::Spi(directory.Path(), test::Maps({
                    {{"type", "image"}, {"image_file", "boot.bin"}, {"read_address", "0x11"},
                     {"write_first_copy_address", "0x22"}, {"write_second_copy_address", "0x33"},
                     {"write_length", "0x44"}, {"erase_first_start_address", "0x55"},
                     {"erase_first_length", "0x66"}, {"erase_second_start_address", "0x77"},
                     {"erase_second_length", "0x88"}}
                }), chip, version);
                REQUIRE(image.Load() == 0);
                REQUIRE(image.GetFlashCommand() == (version == ASTRA_UBOOT_VERSION_2019_10 ?
                    "usbload boot.bin 0x11; spinit; erase 0x55 0x66; cp.b 0x11 0x22 0x44; "
                    "erase 0x77 0x88; cp.b 0x11 0x33 0x44; " :
                    "usbload boot.bin 0x11; sf probe; sf erase 0x55 0x66; sf write 0x11 0x22 0x44; "
                    "sf erase 0x77 0x88; sf write 0x11 0x33 0x44; "));
            });
        }
    }
    suite.Add("version-changes-rebuild-without-accumulation", [] {
        test::TempDirectory directory;
        auto image = test::Spi(directory.Write("boot.bin", "payload"), nullptr, "sl1680",
            ASTRA_UBOOT_VERSION_UNKNOWN);
        REQUIRE(image.Load() == 0 && image.GetFlashCommand() == modernFixed);
        for (int iteration = 0; iteration < 3; ++iteration) {
            image.SetUbootVersion(ASTRA_UBOOT_VERSION_2019_10);
            REQUIRE(image.GetFlashCommand() == legacyFixed);
            image.SetUbootVersion(ASTRA_UBOOT_VERSION_2025_01);
            REQUIRE(image.GetFlashCommand() == modernFixed);
        }
        REQUIRE(image.GetImages().size() == 1 && image.GetFinalImage() == "boot.bin");
    });
    suite.Add("version-changes-preserve-overrides", [] {
        test::TempDirectory directory;
        directory.Write("boot.bin", "payload");
        auto image = test::Spi(directory.Path(), test::Maps({
            {{"type", "image"}, {"image_file", "boot.bin"}, {"read_address", "123"}, {"write_length", "$filesize"}}
        }));
        REQUIRE(image.Load() == 0);
        image.SetUbootVersion(ASTRA_UBOOT_VERSION_2019_10);
        REQUIRE(image.GetFlashCommand() ==
            "usbload boot.bin 123; spinit; erase 0xf0000000 0xf01fffff; cp.b 123 0xf0000000 $filesize; "
            "erase 0xf0200000 0xf03fffff; cp.b 123 0xf0200000 $filesize; ");
        image.SetUbootVersion(ASTRA_UBOOT_VERSION_2025_01);
        REQUIRE(image.GetFlashCommand() ==
            "usbload boot.bin 123; sf probe; sf erase 0 0x200000; sf write 123 0 $filesize; "
            "sf erase 0x200000 0x200000; sf write 123 0x200000 $filesize; ");
    });
    suite.Add("version-change-before-load-no-command", [] {
        test::TempDirectory directory;
        auto image = test::Spi(directory.Write("boot.bin", "payload"));
        image.SetUbootVersion(ASTRA_UBOOT_VERSION_2019_10);
        REQUIRE(image.GetFlashCommand().empty());
        REQUIRE(image.Load() == 0 && image.GetFlashCommand() == legacyFixed);
    });
    for (const std::string type : {"config", "image"}) {
        suite.Add("missing-manifest-image-" + type, [=] {
            test::TempDirectory directory;
            auto image = test::Spi(directory.Path(), test::Maps({
                {{"type", type}, {"image_file", "missing.bin"}}
            }));
            test::RequireLoadError(image, "SPI image file not found:");
            REQUIRE(image.GetFlashCommand().empty() && image.GetImages().empty());
        });
    }
    suite.Add("missing-standalone-file", [] {
        test::TempDirectory directory;
        auto image = test::Spi(directory.Path() / "missing.bin");
        test::RequireLoadError(image, "SPI image path does not exist:");
    });
    suite.Add("unrecognized-or-incomplete-maps-no-images", [] {
        test::TempDirectory directory;
        auto image = test::Spi(directory.Path(), test::Maps({
            {}, {{"type", "other"}, {"image_file", "ignored"}}, {{"type", "image"}}, {{"type", "config"}}
        }));
        test::RequireLoadError(image, "No SPI images configured");
        REQUIRE(image.GetImages().empty());
    });
    for (const std::string name : {"bad name.bin", "bad;name.bin", "nested/boot.bin"}) {
        suite.Add("unsafe-manifest-image-file-" + name, [=] {
            test::TempDirectory directory;
            directory.Write(std::filesystem::path(name), "payload");
            auto image = test::Spi(directory.Path(), test::Maps({
                {{"type", "image"}, {"image_file", name}}
            }));
            test::RequireLoadError(image, "Unsafe image_file in SPI manifest:");
            REQUIRE(image.GetFlashCommand().empty());
        });
    }
    suite.Add("unsafe-standalone-filename", [] {
        test::TempDirectory directory;
        auto image = test::Spi(directory.Write("bad name.bin", "payload"));
        test::RequireLoadError(image, "Unsafe image_file in SPI manifest:");
    });
    for (const char *key : addressKeys) {
        for (const std::string value : {"", "0x", "-1", "1 2", "1;reset"}) {
            suite.Add(std::string("invalid-") + key + "-" + value, [=] {
                test::TempDirectory directory;
                directory.Write("boot.bin", "payload");
                auto image = test::Spi(directory.Path(), test::Maps({
                    {{"type", "image"}, {"image_file", "boot.bin"}, {key, value}}
                }));
                test::RequireLoadError(image, std::string("Unsafe ") + key + " in SPI manifest for boot.bin:");
                REQUIRE(image.GetFlashCommand().empty());
            });
        }
    }
    suite.Add("unrelated-override-not-used-in-command", [] {
        test::TempDirectory directory;
        directory.Write("boot.bin", "payload");
        auto image = test::Spi(directory.Path(), test::Maps({
            {{"type", "image"}, {"image_file", "boot.bin"}, {"unrelated", "not a command token"}}
        }));
        REQUIRE(image.Load() == 0 && image.GetFlashCommand() == modernFixed);
    });
    suite.Add("characterization-repeated-load-appends-images-and-commands", [] {
        test::TempDirectory directory;
        auto image = test::Spi(directory.Write("boot.bin", "payload"));
        REQUIRE(image.Load() == 0);
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetImages().size() == 2);
        REQUIRE(image.GetFlashCommand() == modernFixed + modernFixed);
    });
    return suite.Run(argc, argv);
}
