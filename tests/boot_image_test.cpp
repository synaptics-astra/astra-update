// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <set>

#include "boot_image_fixtures.hpp"
#include "log_fixture.hpp"
#include "unreadable_file_fixture.hpp"

namespace {

AstraBootImage Load(const test::TempDirectory &directory, const test::Manifest &fields)
{
    AstraBootImage image(test::WriteBootManifest(directory, fields).string());
    REQUIRE(image.Load());
    return image;
}

void RequireInvalid(const std::string &yaml)
{
    test::TempDirectory directory;
    directory.Write("manifest.yaml", yaml);
    const auto path = directory.Path() / "errors.log";
    {
        test::LogSession log(path.string(), ASTRA_LOG_LEVEL_ERROR);
        AstraBootImage image(directory.Path().string());
        REQUIRE(!image.Load());
    }
    REQUIRE(test::ReadFile(path).find("[ERROR]") != std::string::npos);
}

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    suite.Add("required-fields-and-optional-defaults", [] {
        test::TempDirectory directory;
        const auto image = Load(directory, test::BootManifest());
        REQUIRE(image.GetID() == "test-image");
        REQUIRE(image.GetChipName() == "sl2619");
        REQUIRE(image.GetBoardName() == "rdk");
        REQUIRE(image.GetVendorId() == 0x06CB && image.GetProductId() == 0x019E);
        REQUIRE(image.GetSysMgrVendorId() == 0 && image.GetSysMgrProductId() == 0);
        REQUIRE(image.GetFastbootVendorId() == 0 && image.GetFastbootProductId() == 0);
        REQUIRE(image.GetVendorProductIdPairs() == std::vector<std::pair<uint16_t, uint16_t>>{{0x06CB, 0x019E}});
        REQUIRE(image.GetSecureBootVersion() == ASTRA_SECURE_BOOT_V3);
        REQUIRE(image.GetMemoryLayout() == ASTRA_MEMORY_LAYOUT_2GB);
        REQUIRE(image.GetMemoryDDRType() == ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED);
        REQUIRE(image.GetUbootConsole() == ASTRA_UBOOT_CONSOLE_UART);
        REQUIRE(image.GetTransportType() == ASTRA_TRANSPORT_USB);
        REQUIRE(image.GetUbootVariant() == ASTRA_UBOOT_VARIANT_SYNAPTICS);
        REQUIRE(image.GetUbootVersion() == ASTRA_UBOOT_VERSION_2025_01);
        REQUIRE(!image.GetUEnvSupport() && !image.GetNandSupport());
        REQUIRE(!image.IsLinuxBoot());
        REQUIRE(image.GetDefaultBootStage() == ASTRA_DEVICE_BOOT_STAGE_AUTO);
        REQUIRE(image.GetFinalBootImage() == "gen3_uboot.bin.usb");
        REQUIRE(image.GetImages().empty());
    });
    for (const auto layout : test::layouts) {
        suite.Add(std::string("layout-") + layout.name, [=] {
            test::TempDirectory directory;
            auto fields = test::BootManifest();
            fields["memory_layout"] = layout.name;
            REQUIRE(Load(directory, fields).GetMemoryLayout() == layout.value);
        });
    }
    for (const auto ddr : test::ddrTypes) {
        suite.Add(std::string("ddr-") + ddr.name, [=] {
            test::TempDirectory directory;
            auto fields = test::BootManifest();
            fields["ddr_type"] = ddr.name;
            REQUIRE(Load(directory, fields).GetMemoryDDRType() == ddr.value);
        });
    }
    suite.Add("normalizes-supported-manifest-values", [] {
        test::TempDirectory directory;
        auto fields = test::BootManifest();
        fields["memory_layout"] = "4GB";
        fields["ddr_type"] = "LPDDR4X";
        fields["secure_boot"] = "GEN2";
        fields["transport"] = "USB_CDC";
        fields["uboot"] = "UBOOT";
        fields["uboot_version"] = "U-BOOT 2019.10.202505221539-gabc (May 21 2025)";
        fields["boot_stage"] = "LINUX";
        fields["chip"] = "SL1680";
        fields["board"] = "RDK";
        const auto image = Load(directory, fields);
        REQUIRE(image.GetMemoryLayout() == ASTRA_MEMORY_LAYOUT_4GB);
        REQUIRE(image.GetMemoryDDRType() == ASTRA_MEMORY_DDR_TYPE_LPDDR4X);
        REQUIRE(image.GetSecureBootVersion() == ASTRA_SECURE_BOOT_V2);
        REQUIRE(image.GetTransportType() == ASTRA_TRANSPORT_USB_CDC);
        REQUIRE(image.GetUbootVariant() == ASTRA_UBOOT_VARIANT_UBOOT);
        REQUIRE(image.GetUbootVersion() == ASTRA_UBOOT_VERSION_2019_10);
        REQUIRE(image.GetDefaultBootStage() == ASTRA_DEVICE_BOOT_STAGE_LINUX);
        REQUIRE(image.GetChipName() == "SL1680" && image.GetBoardName() == "RDK");
    });
    for (const std::string transport : {"usb", "usb_cdc"}) {
        suite.Add("transport-" + transport, [=] {
            test::TempDirectory directory;
            auto fields = test::BootManifest();
            fields["transport"] = transport;
            REQUIRE(Load(directory, fields).GetTransportType() ==
                (transport == "usb" ? ASTRA_TRANSPORT_USB : ASTRA_TRANSPORT_USB_CDC));
        });
    }
    for (const std::string console : {"uart", "usb"}) {
        suite.Add("console-" + console, [=] {
            test::TempDirectory directory;
            auto fields = test::BootManifest();
            fields["console"] = console;
            REQUIRE(Load(directory, fields).GetUbootConsole() ==
                (console == "uart" ? ASTRA_UBOOT_CONSOLE_UART : ASTRA_UBOOT_CONSOLE_USB));
        });
    }
    for (const std::string variant : {"suboot", "uboot", "unknown"}) {
        suite.Add("uboot-variant-" + variant, [=] {
            test::TempDirectory directory;
            auto fields = test::BootManifest();
            fields["uboot"] = variant;
            REQUIRE(Load(directory, fields).GetUbootVariant() ==
                (variant == "suboot" ? ASTRA_UBOOT_VARIANT_SYNAPTICS :
                    variant == "uboot" ? ASTRA_UBOOT_VARIANT_UBOOT : ASTRA_UBOOT_VARIANT_UNKNOWN));
        });
    }
    for (const std::string stage : {"auto", "bootloader", "linux", "m52bl", "sysmgr", "unknown"}) {
        suite.Add("boot-stage-" + stage, [=] {
            test::TempDirectory directory;
            auto fields = test::BootManifest();
            fields["boot_stage"] = stage;
            const std::map<std::string, AstraDeviceBootStage> expected = {
                {"auto", ASTRA_DEVICE_BOOT_STAGE_AUTO}, {"bootloader", ASTRA_DEVICE_BOOT_STAGE_BOOTLOADER},
                {"linux", ASTRA_DEVICE_BOOT_STAGE_LINUX}, {"m52bl", ASTRA_DEVICE_BOOT_STAGE_M52BL},
                {"sysmgr", ASTRA_DEVICE_BOOT_STAGE_SYSMGR}, {"unknown", ASTRA_DEVICE_BOOT_STAGE_AUTO}
            };
            REQUIRE(Load(directory, fields).GetDefaultBootStage() == expected.at(stage));
        });
    }
    for (const std::string version : {"2019.10", "2025.01",
        "U-Boot 2019.10.20250522-gabc (May 21 2025)",
        "U-Boot 2025.01.20260718-gabc (Jul 18 2026)", "2025.01.1234", "2026.01", ""}) {
        suite.Add("uboot-version-" + version, [=] {
            test::TempDirectory directory;
            auto fields = test::BootManifest();
            fields["uboot_version"] = version;
            const auto expected = version.find("2019.10") != std::string::npos ? ASTRA_UBOOT_VERSION_2019_10 :
                version.find("2025.01") != std::string::npos ? ASTRA_UBOOT_VERSION_2025_01 : ASTRA_UBOOT_VERSION_UNKNOWN;
            REQUIRE(Load(directory, fields).GetUbootVersion() == expected);
        });
    }
    suite.Add("all-optional-ids-and-flags", [] {
        test::TempDirectory directory;
        auto fields = test::BootManifest();
        fields["vendor_id"] = "0x06cb";
        fields["product_id"] = "0X019e";
        fields["sysmgr_vendor_id"] = "CAFE";
        fields["sysmgr_product_id"] = "4002";
        fields["fastboot_vendor_id"] = "18D1";
        fields["fastboot_product_id"] = "4EE0";
        fields["nand_support"] = "true";
        fields["uenv_support"] = "true";
        const auto image = Load(directory, fields);
        REQUIRE(image.GetVendorId() == 0x06CB && image.GetProductId() == 0x019E);
        REQUIRE(image.GetSysMgrVendorId() == 0xCAFE && image.GetSysMgrProductId() == 0x4002);
        REQUIRE(image.GetFastbootVendorId() == 0x18D1 && image.GetFastbootProductId() == 0x4EE0);
        REQUIRE(image.GetVendorProductIdPairs() ==
            std::vector<std::pair<uint16_t, uint16_t>>{{0x06CB, 0x019E}, {0xCAFE, 0x4002}});
        REQUIRE(image.GetNandSupport() && image.GetUEnvSupport());
        REQUIRE(image.GetFinalBootImage() == "uEnv.txt");
    });
    for (const std::string optional : {"sysmgr_vendor_id", "sysmgr_product_id",
        "fastboot_vendor_id", "fastboot_product_id"}) {
        suite.Add("incomplete-optional-id-pair-" + optional, [=] {
            test::TempDirectory directory;
            auto fields = test::BootManifest();
            fields[optional] = "1234";
            const auto image = Load(directory, fields);
            REQUIRE(image.GetSysMgrVendorId() == 0 && image.GetSysMgrProductId() == 0);
            REQUIRE(image.GetFastbootVendorId() == 0 && image.GetFastbootProductId() == 0);
            REQUIRE(image.GetVendorProductIdPairs().size() == 1);
        });
    }
    suite.Add("explicit-zero-optional-ids-and-false-flags", [] {
        test::TempDirectory directory;
        auto fields = test::BootManifest();
        fields["sysmgr_vendor_id"] = "0000";
        fields["sysmgr_product_id"] = "0000";
        fields["fastboot_vendor_id"] = "0000";
        fields["fastboot_product_id"] = "0000";
        fields["nand_support"] = "false";
        const auto image = Load(directory, fields);
        REQUIRE(image.GetVendorProductIdPairs().size() == 1);
        REQUIRE(image.GetFastbootVendorId() == 0 && image.GetFastbootProductId() == 0);
        REQUIRE(!image.GetNandSupport());
    });
    for (const std::string secureBoot : {"gen2", "genx"}) {
        for (const bool uEnv : {false, true}) {
            suite.Add("final-image-" + secureBoot + (uEnv ? "-uenv" : "-no-uenv"), [=] {
                test::TempDirectory directory;
                auto fields = test::BootManifest();
                fields["secure_boot"] = secureBoot;
                fields["uenv_support"] = uEnv ? "true" : "false";
                const auto image = Load(directory, fields);
                REQUIRE(!image.IsLinuxBoot());
                REQUIRE(image.GetFinalBootImage() ==
                    (secureBoot == "gen2" ? "minildr.img" : uEnv ? "uEnv.txt" : "gen3_uboot.bin.usb"));
            });
        }
    }
    for (const bool compressed : {false, true}) {
        suite.Add(compressed ? "linux-compressed-kernel" : "linux-uncompressed-kernel", [=] {
            test::TempDirectory directory;
            directory.Write(compressed ? "Image.gz" : "Image", "kernel");
            directory.Write(compressed ? "ramdisk.cpio.gz" : "rootfs.cpio.gz", "ramdisk");
            const auto image = Load(directory, test::BootManifest());
            REQUIRE(image.IsLinuxBoot());
            REQUIRE(image.GetFinalBootImage() == (compressed ? "ramdisk.cpio.gz" : "rootfs.cpio.gz"));
            REQUIRE(image.GetImages().size() == 2);
        });
    }
    suite.Add("linux-layout-precedence", [] {
        test::TempDirectory directory;
        directory.Write("Image.gz", "kernel");
        directory.Write("ramdisk.cpio.gz", "ramdisk");
        directory.Write("Image", "kernel");
        directory.Write("rootfs.cpio.gz", "rootfs");
        auto fields = test::BootManifest();
        fields["secure_boot"] = "gen2";
        fields["uenv_support"] = "true";
        const auto image = Load(directory, fields);
        REQUIRE(image.IsLinuxBoot());
        REQUIRE(image.GetFinalBootImage() == "ramdisk.cpio.gz");
    });
    for (const std::string file : {"Image.gz", "Image", "ramdisk.cpio.gz", "rootfs.cpio.gz"}) {
        suite.Add("incomplete-linux-layout-" + file, [=] {
            test::TempDirectory directory;
            directory.Write(file, "data");
            const auto image = Load(directory, test::BootManifest());
            REQUIRE(!image.IsLinuxBoot());
            REQUIRE(image.GetFinalBootImage() == "gen3_uboot.bin.usb");
        });
    }
    suite.Add("nested-payloads-have-relative-names", [] {
        test::TempDirectory directory;
        directory.Write("top.bin", "top");
        directory.Write(std::filesystem::path("nested") / "payload.bin", std::string("a\0b", 3));
        directory.Write(std::filesystem::path("nested") / "manifest.yaml", "nested payload");
        std::filesystem::create_directory(directory.Path() / "empty");
        auto image = Load(directory, test::BootManifest());
        std::set<std::string> names;
        for (auto payload : image.GetImages()) {
            REQUIRE(payload.GetImageType() == ASTRA_IMAGE_TYPE_BOOT);
            REQUIRE(std::filesystem::is_regular_file(payload.GetPath()));
            REQUIRE(payload.Load() == 0);
            names.insert(payload.GetName());
        }
        REQUIRE(names == std::set<std::string>{"top.bin", "nested/payload.bin", "nested/manifest.yaml"});
    });
    for (const auto &field : test::BootManifest()) {
        suite.Add("missing-required-field-" + field.first, [key = field.first] {
            auto fields = test::BootManifest();
            fields.erase(key);
            RequireInvalid(test::ManifestYaml(fields));
        });
    }
    for (const std::string yaml : {"id: [", "[]", "scalar", "id: {wrong: shape}"}) {
        suite.Add("malformed-or-wrong-shaped-yaml-" + yaml, [=] { RequireInvalid(yaml); });
    }
    const test::Manifest invalidValues = {
        {"memory_layout", "8gb"}, {"transport", "serial"}, {"vendor_id", "invalid"},
        {"product_id", ""}, {"uenv_support", "maybe"}, {"nand_support", "maybe"}
    };
    for (const auto &field : invalidValues) {
        suite.Add("invalid-field-" + field.first, [=] {
            auto fields = test::BootManifest();
            fields[field.first] = field.second;
            RequireInvalid(test::ManifestYaml(fields));
        });
    }
    suite.Add("wrong-typed-required-value", [] {
        auto fields = test::BootManifest();
        fields.erase("chip");
        RequireInvalid(test::ManifestYaml(fields) + "chip: [sl2619]\n");
    });
    suite.Add("missing-path-or-regular-file-rejected", [] {
        test::TempDirectory directory;
        AstraBootImage missing((directory.Path() / "missing").string());
        REQUIRE(!missing.Load());
        AstraBootImage file(directory.Write("file.bin", "not a directory").string());
        REQUIRE(!file.Load());
    });
    suite.Add("unreadable-manifest-reports-failure", [] {
        test::TempDirectory directory;
        test::WriteBootManifest(directory, test::BootManifest());
        const auto logPath = directory.Path() / "error.log";
        {
            test::UnreadableFile unreadable(directory.Path() / "manifest.yaml");
            unreadable.RequireUnreadable();
            test::LogSession session(logPath.string(), ASTRA_LOG_LEVEL_ERROR);
            AstraBootImage image(directory.Path().string());
            REQUIRE(!image.Load());
        }
        REQUIRE(test::ReadFile(logPath).find("Unable to open the manifest file") != std::string::npos);
    });
    suite.Add("characterization-unknown-value-defaults", [] {
        test::TempDirectory directory;
        auto fields = test::BootManifest();
        fields["secure_boot"] = "unknown";
        fields["ddr_type"] = "unknown";
        fields["console"] = "UART";
        fields["boot_stage"] = "unknown";
        const auto image = Load(directory, fields);
        REQUIRE(image.GetSecureBootVersion() == ASTRA_SECURE_BOOT_V3);
        REQUIRE(image.GetMemoryDDRType() == ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED);
        REQUIRE(image.GetUbootConsole() == ASTRA_UBOOT_CONSOLE_USB);
        REQUIRE(image.GetDefaultBootStage() == ASTRA_DEVICE_BOOT_STAGE_AUTO);
    });
    suite.Add("characterization-repeated-load-appends-payloads", [] {
        test::TempDirectory directory;
        directory.Write("payload.bin", "data");
        auto image = Load(directory, test::BootManifest());
        REQUIRE(image.Load());
        REQUIRE(image.GetImages().size() == 2);
    });
    return suite.Run(argc, argv);
}
