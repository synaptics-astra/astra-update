// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <array>
#include <algorithm>

#include "image.hpp"
#include "log_fixture.hpp"
#include "test_support.hpp"
#include "unreadable_file_fixture.hpp"

int main(int argc, char **argv)
{
    test::Suite suite;
    suite.Add("name-path-and-type", [] {
        for (const auto type : {ASTRA_IMAGE_TYPE_BOOT, ASTRA_IMAGE_TYPE_UPDATE_EMMC,
            ASTRA_IMAGE_TYPE_UPDATE_SPI, ASTRA_IMAGE_TYPE_UPDATE_NAND}) {
            const auto path = std::filesystem::path("folder") / "payload.bin";
            Image image(path.string(), type);
            REQUIRE(image.GetName() == "payload.bin");
            REQUIRE(image.GetPath() == path.string());
            REQUIRE(image.GetImageType() == type);
            REQUIRE(image.GetSize() == 0);
            Image named(path.string(), type, "nested/payload");
            REQUIRE(named.GetName() == "nested/payload");
        }
    });
    suite.Add("read-before-load-and-missing-file-diagnostics", [] {
        test::TempDirectory directory;
        const auto logPath = directory.Path() / "error.log";
        {
            test::LogSession session(logPath.string(), ASTRA_LOG_LEVEL_ERROR);
            Image image((directory.Path() / "missing.bin").string(), ASTRA_IMAGE_TYPE_BOOT);
            uint8_t byte = 0;
            REQUIRE(image.GetDataBlock(&byte, 1) == -1);
            REQUIRE(image.Load() == -1);
            REQUIRE(image.GetSize() == 0);
        }
        const auto errors = test::ReadFile(logPath);
        REQUIRE(errors.find("Image not loaded") != std::string::npos);
        REQUIRE(errors.find("Image file does not exist") != std::string::npos);
    });
    suite.Add("empty-file-and-zero-read", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("empty.bin", "");
        Image image(path.string(), ASTRA_IMAGE_TYPE_BOOT);
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetSize() == 0);
        uint8_t byte = 0xFF;
        REQUIRE(image.GetDataBlock(&byte, 0) == 0);
        REQUIRE(image.GetDataBlock(&byte, 1) == 0);
        REQUIRE(byte == 0xFF);
    });
    suite.Add("binary-chunked-reads-zero-read-and-eof", [] {
        test::TempDirectory directory;
        const std::string data("\0\x01\x7F\x80\xFF", 5);
        Image image(directory.Write("binary.bin", data).string(), ASTRA_IMAGE_TYPE_BOOT);
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetSize() == data.size());
        std::array<uint8_t, 8> buffer{};
        REQUIRE(image.GetDataBlock(buffer.data(), 0) == 0);
        REQUIRE(image.GetDataBlock(buffer.data(), 2) == 2);
        REQUIRE(buffer[0] == 0 && buffer[1] == 1);
        std::fill(buffer.begin(), buffer.end(), 0xCC);
        REQUIRE(image.GetDataBlock(buffer.data(), buffer.size()) == 3);
        REQUIRE(buffer[0] == 0x7F && buffer[1] == 0x80 && buffer[2] == 0xFF);
        REQUIRE(buffer[3] == 0xCC);
        REQUIRE(image.GetDataBlock(buffer.data(), buffer.size()) == 0);
        REQUIRE(image.GetDataBlock(buffer.data(), 1) == 0);
    });
    suite.Add("reload-rewinds", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("reload.bin", "abcd");
        Image image(path.string(), ASTRA_IMAGE_TYPE_BOOT);
        REQUIRE(image.Load() == 0);
        uint8_t byte = 0;
        REQUIRE(image.GetDataBlock(&byte, 1) == 1);
        REQUIRE(byte == 'a');
        REQUIRE(image.Load() == 0);
        REQUIRE(image.GetDataBlock(&byte, 1) == 1 && byte == 'a');
    });
    suite.Add("directory-is-not-an-image-file", [] {
        test::TempDirectory directory;
        Image image(directory.Path().string(), ASTRA_IMAGE_TYPE_BOOT);
        REQUIRE(image.Load() == -1);
    });
    suite.Add("unreadable-image-reports-open-failure", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("unreadable.bin", "data");
        const auto logPath = directory.Path() / "error.log";
        {
            test::UnreadableFile unreadable(path);
            unreadable.RequireUnreadable();
            test::LogSession session(logPath.string(), ASTRA_LOG_LEVEL_ERROR);
            Image image(path.string(), ASTRA_IMAGE_TYPE_BOOT);
            REQUIRE(image.Load() == -1);
        }
        REQUIRE(test::ReadFile(logPath).find("Failed to open file") != std::string::npos);
    });
    suite.Add("truncated-loaded-file-reports-short-read", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("truncate.bin", std::string(8192, 'x'));
        const auto logPath = directory.Path() / "error.log";
        {
            test::LogSession session(logPath.string(), ASTRA_LOG_LEVEL_ERROR);
            Image image(path.string(), ASTRA_IMAGE_TYPE_BOOT);
            REQUIRE(image.Load() == 0);
            std::filesystem::resize_file(path, 4096);
            std::array<uint8_t, 8192> buffer{};
            REQUIRE(image.GetDataBlock(buffer.data(), buffer.size()) == -1);
            REQUIRE(image.GetSize() == 8192);
        }
        const auto errors = test::ReadFile(logPath);
        REQUIRE(errors.find("Short read from") != std::string::npos);
        REQUIRE(errors.find("expected 8192 bytes, got 4096") != std::string::npos);
    });
    suite.Add("copies-share-cursor-and-outlive-source", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("shared.bin", "abcd");
        Image survivor(path.string(), ASTRA_IMAGE_TYPE_BOOT);
        uint8_t byte = 0;
        {
            Image original(path.string(), ASTRA_IMAGE_TYPE_BOOT);
            REQUIRE(original.Load() == 0);
            survivor = original;
            REQUIRE(original.GetDataBlock(&byte, 1) == 1 && byte == 'a');
            Image copy(original);
            REQUIRE(copy.GetDataBlock(&byte, 1) == 1 && byte == 'b');
        }
        REQUIRE(survivor.GetDataBlock(&byte, 1) == 1 && byte == 'c');
        REQUIRE(survivor.GetSize() == 4);
    });
    suite.Add("reload-detaches-copy-handle", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("reload.bin", "abc");
        Image original(path.string(), ASTRA_IMAGE_TYPE_BOOT);
        REQUIRE(original.Load() == 0);
        Image copy(original);
        uint8_t byte = 0;
        REQUIRE(original.GetDataBlock(&byte, 1) == 1 && byte == 'a');
        REQUIRE(original.Load() == 0);
        REQUIRE(original.GetDataBlock(&byte, 1) == 1 && byte == 'a');
        REQUIRE(copy.GetDataBlock(&byte, 1) == 1 && byte == 'b');
    });
    suite.Add("move-and-vector-reallocation-preserve-handle", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("move.bin", "xyz");
        Image source(path.string(), ASTRA_IMAGE_TYPE_UPDATE_EMMC);
        REQUIRE(source.Load() == 0);
        Image moved(std::move(source));
        Image assigned(path.string(), ASTRA_IMAGE_TYPE_BOOT);
        assigned = std::move(moved);
        std::vector<Image> images;
        images.push_back(std::move(assigned));
        for (int i = 0; i < 64; ++i) {
            images.emplace_back(path.string(), ASTRA_IMAGE_TYPE_BOOT);
        }
        uint8_t byte = 0;
        REQUIRE(images.front().GetDataBlock(&byte, 1) == 1 && byte == 'x');
        REQUIRE(images.front().GetImageType() == ASTRA_IMAGE_TYPE_UPDATE_EMMC);
    });
    return suite.Run(argc, argv);
}
