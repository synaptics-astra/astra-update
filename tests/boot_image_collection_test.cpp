// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <algorithm>
#include <set>

#include "boot_image_collection.hpp"
#include "boot_image_fixtures.hpp"
#include "flash_image.hpp"

namespace {

constexpr test::LayoutCase orderedLayouts[] = {
    {"512mb", ASTRA_MEMORY_LAYOUT_512MB}, {"1gb", ASTRA_MEMORY_LAYOUT_1GB},
    {"2gb", ASTRA_MEMORY_LAYOUT_2GB}, {"3gb", ASTRA_MEMORY_LAYOUT_3GB},
    {"4gb", ASTRA_MEMORY_LAYOUT_4GB}
};

size_t LayoutRank(AstraMemoryLayout layout)
{
    const auto found = std::find_if(std::begin(orderedLayouts), std::end(orderedLayouts),
        [layout](const test::LayoutCase &entry) { return entry.value == layout; });
    REQUIRE(found != std::end(orderedLayouts));
    return static_cast<size_t>(found - std::begin(orderedLayouts));
}

class MatchingFixture
{
public:
    MatchingFixture() : collection(directory.Path().string())
    {
        for (const std::string chip : {"sl2610", "sl2619"}) {
            for (const std::string board : {"coralboard", "rdk"}) {
                for (const std::string ddr : {"ddr4", "lpddr4", ""}) {
                    Add(chip + "-" + board + "-" + ddr, chip, board, ddr);
                }
            }
        }
        Add("fallback-only", "sl2610", "fallback-board", "ddr4");
        Add("wrong-chip", "sl1680", "rdk", "ddr4");
        Add("wrong-secure-boot", "sl2619", "rdk", "ddr4", "gen2");
        Add("wrong-memory-layout", "sl2619", "rdk", "ddr4", "genx", "4gb");
        collection.Load();
        REQUIRE(collection.GetDeviceIDs().size() == 16);
    }

    void Check(const std::string &chip, const std::string &board,
        AstraMemoryDDRType ddr, const std::set<std::string> &expected) const
    {
        const auto images = collection.GetBootImagesForChip(chip, ASTRA_SECURE_BOOT_V3,
            ASTRA_MEMORY_LAYOUT_2GB, ddr, board);
        std::set<std::string> actual;
        for (const auto &image : images) {
            actual.insert(image->GetID());
        }
        if (actual != expected) {
            std::ostringstream message;
            message << "Expected:";
            for (const auto &id : expected) {
                message << ' ' << id;
            }
            message << "; actual:";
            for (const auto &id : actual) {
                message << ' ' << id;
            }
            throw std::runtime_error(message.str());
        }
        REQUIRE(images.size() == expected.size());
    }

    test::TempDirectory directory;
    BootImageCollection collection;

private:
    void Add(const std::string &id, const std::string &chip, const std::string &board,
        const std::string &ddr, const std::string &secureBoot = "genx",
        const std::string &layout = "2gb")
    {
        auto fields = test::BootManifest(id);
        fields["chip"] = chip;
        fields["board"] = board;
        fields["memory_layout"] = layout;
        fields["secure_boot"] = secureBoot;
        if (!ddr.empty()) {
            fields["ddr_type"] = ddr;
        }
        test::WriteBootManifest(directory, fields, id);
    }
};

void AddMatchingRegressions(test::Suite &suite)
{
    for (const std::string chip : {"sl2619", "sl2618", "sl2610"}) {
        const std::string prefix = chip == "sl2619" ? "sl2619" : "sl2610";
        auto add = [&](const std::string &name, const std::string &board,
            AstraMemoryDDRType ddr, std::set<std::string> expected) {
            suite.Add(chip + "-" + name, [=] {
                MatchingFixture fixture;
                fixture.Check(chip, board, ddr, expected);
            });
        };
        add("board-and-ddr-match", "rdk", ASTRA_MEMORY_DDR_TYPE_DDR4, {prefix + "-rdk-ddr4"});
        add("board-match-ddr-omitted", "rdk", ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED,
            {prefix + "-rdk-ddr4", prefix + "-rdk-lpddr4", prefix + "-rdk-"});
        add("board-mismatch-ddr-match", "unknown-board", ASTRA_MEMORY_DDR_TYPE_DDR4, {});
        add("board-mismatch-ddr-omitted", "unknown-board", ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, {});
        add("board-and-ddr-mismatch", "unknown-board", ASTRA_MEMORY_DDR_TYPE_DDR3, {});
        add("board-match-ddr-mismatch", "rdk", ASTRA_MEMORY_DDR_TYPE_DDR3, {});
        add("board-omitted-ddr-match", "", ASTRA_MEMORY_DDR_TYPE_LPDDR4,
            {prefix + "-rdk-lpddr4", prefix + "-coralboard-lpddr4"});
        add("board-omitted-ddr-mismatch", "", ASTRA_MEMORY_DDR_TYPE_DDR3, {});
        add("board-any-ddr-match", "any", ASTRA_MEMORY_DDR_TYPE_LPDDR4,
            {prefix + "-rdk-lpddr4", prefix + "-coralboard-lpddr4"});
        add("board-any-ddr-mismatch", "any", ASTRA_MEMORY_DDR_TYPE_DDR3, {});
        std::set<std::string> allImages = {
            prefix + "-rdk-ddr4", prefix + "-rdk-lpddr4", prefix + "-rdk-",
            prefix + "-coralboard-ddr4", prefix + "-coralboard-lpddr4", prefix + "-coralboard-"
        };
        if (prefix == "sl2610") {
            allImages.insert("fallback-only");
        }
        add("board-and-ddr-omitted", "", ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, allImages);
        add("board-any-ddr-omitted", "any", ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, allImages);
    }
    suite.Add("board-mismatch-allows-fallback", [] {
        MatchingFixture fixture;
        fixture.Check("sl2619", "fallback-board", ASTRA_MEMORY_DDR_TYPE_DDR4, {"fallback-only"});
    });
    suite.Add("board-mismatch-allows-fallback-ddr-omitted", [] {
        MatchingFixture fixture;
        fixture.Check("sl2619", "fallback-board", ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, {"fallback-only"});
    });
    suite.Add("no-fallback-outside-sl261x", [] {
        MatchingFixture fixture;
        fixture.Check("sl1680", "fallback-board", ASTRA_MEMORY_DDR_TYPE_DDR4, {});
    });
}

void AddXspiRegressions(test::Suite &suite)
{
    for (const auto type : {FLASH_IMAGE_TYPE_SPI, FLASH_IMAGE_TYPE_EMMC, FLASH_IMAGE_TYPE_NAND}) {
        for (unsigned int mask = 1; mask < 8; ++mask) {
            suite.Add("xspi-selection-" + std::to_string(type) + "-" + std::to_string(mask), [=] {
                test::TempDirectory directory;
                const std::vector<std::string> ids{"legacy", "non-xspi", "xspi"};
                for (size_t i = 0; i < ids.size(); ++i) {
                    if ((mask & (1u << i)) == 0) {
                        continue;
                    }
                    auto fields = test::BootManifest(ids[i]);
                    fields["nand_support"] = "true";
                    if (i != 0) {
                        fields["xspi_support"] = i == 2 ? "true" : "false";
                    }
                    test::WriteBootManifest(directory, fields, ids[i]);
                }
                BootImageCollection collection(directory.Path().string());
                collection.Load();
                const auto images = collection.GetBootImagesForChip("sl2619", ASTRA_SECURE_BOOT_V3,
                    ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, "rdk", type);
                std::set<std::string> actual;
                for (const auto &image : images) {
                    actual.insert(image->GetID());
                }
                std::set<std::string> expected;
                if (type == FLASH_IMAGE_TYPE_SPI) {
                    if (mask & 4u) {
                        expected.insert("xspi");
                    } else if (mask & 1u) {
                        expected.insert("legacy");
                    }
                } else {
                    if (mask & 1u) {
                        expected.insert("legacy");
                    }
                    if (mask & 2u) {
                        expected.insert("non-xspi");
                    }
                    if (expected.empty()) {
                        expected.insert("xspi");
                    }
                }
                REQUIRE(actual == expected);
            });
        }
    }
    suite.Add("xspi-filter-allows-compatible-chip-fallback", [] {
        test::TempDirectory directory;
        auto exact = test::BootManifest("exact");
        exact["xspi_support"] = "false";
        test::WriteBootManifest(directory, exact, "exact");
        auto fallback = test::BootManifest("fallback");
        fallback["chip"] = "sl2610";
        fallback["xspi_support"] = "true";
        test::WriteBootManifest(directory, fallback, "fallback");
        BootImageCollection collection(directory.Path().string());
        collection.Load();
        const auto images = collection.GetBootImagesForChip("sl2619", ASTRA_SECURE_BOOT_V3,
            ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, "rdk", FLASH_IMAGE_TYPE_SPI);
        REQUIRE(images.size() == 1);
        REQUIRE(images.front()->GetID() == "fallback");
    });
    suite.Add("xspi-preference-preserves-exact-chip-priority", [] {
        test::TempDirectory directory;
        test::WriteBootManifest(directory, test::BootManifest("exact"), "exact");
        auto fallback = test::BootManifest("fallback");
        fallback["chip"] = "sl2610";
        fallback["xspi_support"] = "true";
        test::WriteBootManifest(directory, fallback, "fallback");
        BootImageCollection collection(directory.Path().string());
        collection.Load();
        const auto images = collection.GetBootImagesForChip("sl2619", ASTRA_SECURE_BOOT_V3,
            ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, "rdk", FLASH_IMAGE_TYPE_SPI);
        REQUIRE(images.size() == 1);
        REQUIRE(images.front()->GetID() == "exact");
    });
    suite.Add("nand-capability-is-filtered-before-xspi-preference", [] {
        test::TempDirectory directory;
        auto nonNand = test::BootManifest("non-nand");
        nonNand["xspi_support"] = "false";
        test::WriteBootManifest(directory, nonNand, "non-nand");
        auto nand = test::BootManifest("nand");
        nand["xspi_support"] = "true";
        nand["nand_support"] = "true";
        test::WriteBootManifest(directory, nand, "nand");
        BootImageCollection collection(directory.Path().string());
        collection.Load();
        const auto images = collection.GetBootImagesForChip("sl2619", ASTRA_SECURE_BOOT_V3,
            ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, "rdk", FLASH_IMAGE_TYPE_NAND);
        REQUIRE(images.size() == 1);
        REQUIRE(images.front()->GetID() == "nand");
    });
    suite.Add("chip-query-without-update-type-keeps-all-xspi-variants", [] {
        test::TempDirectory directory;
        for (const std::string support : {"true", "false"}) {
            auto fields = test::BootManifest(support);
            fields["xspi_support"] = support;
            test::WriteBootManifest(directory, fields, support);
        }
        BootImageCollection collection(directory.Path().string());
        collection.Load();
        REQUIRE(collection.GetBootImagesForChip("sl2619", ASTRA_SECURE_BOOT_V3,
            ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, "rdk").size() == 2);
    });
}

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    AddMatchingRegressions(suite);
    AddXspiRegressions(suite);
    for (const bool overrideBoard : {false, true}) {
        suite.Add(overrideBoard ? "config-any-board-overrides-manifest" : "manifest-any-board", [=] {
            MatchingFixture fixture;
            auto fields = test::BootManifest("update");
            fields["board"] = overrideBoard ? "rdk" : "any";
            fields["ddr_type"] = "lpddr4";
            const auto path = test::WriteBootManifest(fixture.directory, fields, "update");
            test::Manifest config{{"image_type", "spi"}};
            if (overrideBoard) {
                config["board"] = "ANY";
            }
            const auto image = FlashImage::FlashImageFactory(path.string(), config);
            REQUIRE(image->GetBoardName() == "any");
            fixture.Check(image->GetChipName(), image->GetBoardName(), image->GetMemoryDDRType(),
                {"sl2619-rdk-lpddr4", "sl2619-coralboard-lpddr4"});
        });
    }
    suite.Add("missing-collection-path", [] {
        test::TempDirectory directory;
        BootImageCollection collection((directory.Path() / "missing").string());
        REQUIRE_THROWS(std::invalid_argument, collection.Load());
    });
    suite.Add("empty-collection-and-missing-id", [] {
        test::TempDirectory directory;
        BootImageCollection collection(directory.Path().string());
        collection.Load();
        REQUIRE(collection.GetDeviceIDs().empty());
        REQUIRE(collection.GetBootImagesForChip("sl2619", ASTRA_SECURE_BOOT_V3,
            ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, "").empty());
        REQUIRE_THROWS(std::runtime_error, collection.GetBootImage("missing"));
    });
    suite.Add("regular-file-collection-path-is-ignored", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("file.bin", "not a collection");
        BootImageCollection collection(path.string());
        collection.Load();
        REQUIRE(collection.GetDeviceIDs().empty());
    });
    suite.Add("loads-only-valid-child-manifests", [] {
        test::TempDirectory directory;
        auto fields = test::BootManifest("good");
        test::WriteBootManifest(directory, fields, "good-child");
        directory.Write(std::filesystem::path("no-manifest") / "payload.bin", "data");
        directory.Write(std::filesystem::path("invalid") / "manifest.yaml", "id: [");
        directory.Write("regular-file", "ignored");
        BootImageCollection collection(directory.Path().string());
        collection.Load();
        REQUIRE(collection.GetDeviceIDs() ==
            std::vector<std::tuple<uint16_t, uint16_t>>{{0x06CB, 0x019E}});
        REQUIRE(collection.GetBootImage("good").GetChipName() == "sl2619");
        REQUIRE_THROWS(std::runtime_error, collection.GetBootImage("invalid"));
    });
    suite.Add("id-lookup-and-primary-device-ids", [] {
        test::TempDirectory directory;
        auto first = test::BootManifest("first");
        first["sysmgr_vendor_id"] = "CAFE";
        first["sysmgr_product_id"] = "4002";
        first["fastboot_vendor_id"] = "18D1";
        first["fastboot_product_id"] = "4EE0";
        test::WriteBootManifest(directory, first, "first");
        auto second = test::BootManifest("second");
        second["product_id"] = "00B1";
        test::WriteBootManifest(directory, second, "second");
        BootImageCollection collection(directory.Path().string());
        collection.Load();
        REQUIRE(collection.GetBootImage("first").GetID() == "first");
        REQUIRE(collection.GetBootImage("second").GetProductId() == 0x00B1);
        auto ids = collection.GetDeviceIDs();
        std::sort(ids.begin(), ids.end());
        REQUIRE(ids == std::vector<std::tuple<uint16_t, uint16_t>>{{0x06CB, 0x00B1}, {0x06CB, 0x019E}});
        REQUIRE_THROWS(std::runtime_error, collection.GetBootImage("FIRST"));
    });
    suite.Add("characterization-repeated-load-appends", [] {
        MatchingFixture fixture;
        fixture.collection.Load();
        REQUIRE(fixture.collection.GetDeviceIDs().size() == 32);
    });
    suite.Add("characterization-direct-image-directory-not-loaded", [] {
        test::TempDirectory directory;
        test::WriteBootManifest(directory, test::BootManifest());
        BootImageCollection collection(directory.Path().string());
        collection.Load();
        REQUIRE(collection.GetDeviceIDs().empty());
    });
    suite.Add("characterization-duplicate-ids-retained", [] {
        test::TempDirectory directory;
        test::WriteBootManifest(directory, test::BootManifest("same"), "a");
        test::WriteBootManifest(directory, test::BootManifest("same"), "b");
        BootImageCollection collection(directory.Path().string());
        collection.Load();
        REQUIRE(collection.GetDeviceIDs().size() == 2);
        REQUIRE(collection.GetBootImage("same").GetID() == "same");
    });
    for (const std::string chip : {"", "SL2619", "sl261", "sl26199", "sl2620", "sr1000"}) {
        suite.Add("no-unintended-chip-match-" + chip, [=] {
            MatchingFixture fixture;
            fixture.Check(chip, "rdk", ASTRA_MEMORY_DDR_TYPE_DDR4, {});
        });
    }
    suite.Add("board-match-is-case-sensitive", [] {
        MatchingFixture fixture;
        fixture.Check("sl2619", "RDK", ASTRA_MEMORY_DDR_TYPE_DDR4, {});
    });
    for (const std::string secureBoot : {"gen2", "genx"}) {
        const auto version = secureBoot == "gen2" ? ASTRA_SECURE_BOOT_V2 : ASTRA_SECURE_BOOT_V3;
        for (const auto layout : test::layouts) {
            for (const auto ddr : test::ddrTypes) {
                suite.Add(secureBoot + "-" + layout.name + "-" + ddr.name + "-constraint-matrix", [=] {
                    test::TempDirectory directory;
                    auto exact = test::BootManifest("exact");
                    exact["secure_boot"] = secureBoot;
                    exact["memory_layout"] = layout.name;
                    exact["ddr_type"] = ddr.name;
                    test::WriteBootManifest(directory, exact, "exact");
                    auto fallback = exact;
                    fallback["id"] = "fallback";
                    fallback["chip"] = "sl2610";
                    test::WriteBootManifest(directory, fallback, "fallback");
                    BootImageCollection collection(directory.Path().string());
                    collection.Load();
                    auto matches = collection.GetBootImagesForChip("sl2619", version, layout.value, ddr.value, "rdk");
                    REQUIRE(matches.size() == 1);
                    REQUIRE(matches.front()->GetID() == "exact");
                    matches = collection.GetBootImagesForChip("sl2618", version, layout.value, ddr.value, "rdk");
                    REQUIRE(matches.size() == 1);
                    REQUIRE(matches.front()->GetID() == "fallback");
                    REQUIRE(collection.GetBootImagesForChip("sl2619",
                        version == ASTRA_SECURE_BOOT_V2 ? ASTRA_SECURE_BOOT_V3 : ASTRA_SECURE_BOOT_V2,
                        layout.value, ddr.value, "rdk").empty());
                    for (const auto other : test::layouts) {
                        const auto compatible = collection.GetBootImagesForChip("sl2619", version,
                            other.value, ddr.value, "rdk");
                        REQUIRE(compatible.size() == (LayoutRank(layout.value) <= LayoutRank(other.value) ? 1u : 0u));
                        if (!compatible.empty()) {
                            REQUIRE(compatible.front()->GetID() == "exact");
                        }
                    }
                    for (const auto other : test::ddrTypes) {
                        if (other.value != ddr.value) {
                            REQUIRE(collection.GetBootImagesForChip("sl2619", version, layout.value, other.value, "rdk").empty());
                        }
                    }
                });
            }
        }
    }
    for (const std::string constraint : {"secure_boot", "memory_layout", "ddr_type"}) {
        suite.Add("exact-mismatch-falls-back-" + constraint, [=] {
            test::TempDirectory directory;
            auto fields = test::BootManifest("fallback");
            fields["chip"] = "sl2610";
            fields["ddr_type"] = "ddr4";
            test::WriteBootManifest(directory, fields, "fallback");
            fields["id"] = "exact";
            fields["chip"] = "sl2619";
            fields[constraint] = constraint == "secure_boot" ? "gen2" :
                constraint == "memory_layout" ? "4gb" : "ddr3";
            test::WriteBootManifest(directory, fields, "exact");
            BootImageCollection collection(directory.Path().string());
            collection.Load();
            const auto matches = collection.GetBootImagesForChip("sl2619", ASTRA_SECURE_BOOT_V3,
                ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_DDR4, "rdk");
            REQUIRE(matches.size() == 1);
            REQUIRE(matches.front()->GetID() == "fallback");
        });
    }
    for (const auto target : orderedLayouts) {
        suite.Add(std::string("compatible-memory-layouts-") + target.name, [=] {
            for (const std::string bootChip : {"sl2619", "sl2610"}) {
                test::TempDirectory directory;
                for (const auto boot : orderedLayouts) {
                    auto fields = test::BootManifest(boot.name);
                    fields["chip"] = bootChip;
                    fields["memory_layout"] = boot.name;
                    test::WriteBootManifest(directory, fields, boot.name);
                }
                BootImageCollection collection(directory.Path().string());
                collection.Load();
                const auto matches = collection.GetBootImagesForChip("sl2619", ASTRA_SECURE_BOOT_V3,
                    target.value, ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, "rdk");
                std::set<std::string> actual;
                for (const auto &image : matches) {
                    actual.insert(image->GetID());
                }
                std::set<std::string> expected;
                for (const auto boot : orderedLayouts) {
                    if (LayoutRank(boot.value) <= LayoutRank(target.value)) {
                        expected.insert(boot.name);
                    }
                }
                REQUIRE(actual == expected);
                REQUIRE(matches.size() == expected.size());
            }
        });
    }
    suite.Add("smaller-exact-chip-retains-priority-over-exact-memory-fallback", [] {
        test::TempDirectory directory;
        auto exact = test::BootManifest("small-exact-chip");
        exact["memory_layout"] = "512mb";
        test::WriteBootManifest(directory, exact, "exact");
        auto fallback = test::BootManifest("large-fallback-chip");
        fallback["chip"] = "sl2610";
        test::WriteBootManifest(directory, fallback, "fallback");
        BootImageCollection collection(directory.Path().string());
        collection.Load();
        const auto matches = collection.GetBootImagesForChip("sl2619", ASTRA_SECURE_BOOT_V3,
            ASTRA_MEMORY_LAYOUT_2GB, ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, "rdk");
        REQUIRE(matches.size() == 1);
        REQUIRE(matches.front()->GetID() == "small-exact-chip");
    });
    suite.Add("invalid-target-memory-layout-is-rejected", [] {
        test::TempDirectory directory;
        BootImageCollection collection(directory.Path().string());
        REQUIRE_THROWS(std::invalid_argument, collection.GetBootImagesForChip("sl2619",
            ASTRA_SECURE_BOOT_V3, static_cast<AstraMemoryLayout>(-1),
            ASTRA_MEMORY_DDR_TYPE_NOT_SPECIFIED, "rdk"));
    });
    return suite.Run(argc, argv);
}
