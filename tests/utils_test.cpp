// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <set>

#include "utils.hpp"
#include "test_support.hpp"

namespace {

class GeneratedDirectories
{
public:
    ~GeneratedDirectories()
    {
        for (const auto &path : paths) {
            if (path.empty()) {
                continue;
            }
            std::error_code error;
            std::filesystem::remove_all(path, error);
            if (error) {
                ++test::cleanupFailures;
                std::cerr << "Generated directory cleanup failed: " << error.message() << '\n';
            }
        }
    }
    std::vector<std::filesystem::path> paths;
};

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    for (const uint32_t value : {0U, 1U, 0x12345678U, 0x80000000U, 0xFFFFFFFFU}) {
        suite.Add("little-endian-" + std::to_string(value), [=] {
            const uint32_t converted = HostToLE(value);
            std::array<uint8_t, 4> bytes{};
            std::memcpy(bytes.data(), &converted, sizeof(converted));
            REQUIRE(bytes == std::array<uint8_t, 4>{
                static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)});
        });
    }
    suite.Add("temp-directories-exist-and-are-unique", [] {
        GeneratedDirectories generated;
        for (int i = 0; i < 16; ++i) {
            generated.paths.emplace_back(MakeTempDirectory());
            REQUIRE(!generated.paths.back().empty());
            REQUIRE(std::filesystem::is_directory(generated.paths.back()));
        }
        REQUIRE(std::set<std::filesystem::path>(generated.paths.begin(), generated.paths.end()).size() == 16);
    });
    suite.Add("concurrent-temp-directory-creation", [] {
        GeneratedDirectories generated;
        std::mutex mutex;
        std::exception_ptr failure;
        test::ThreadGroup threads;
        for (int worker = 0; worker < 4; ++worker) {
            threads.Add([&] {
                try {
                    for (int i = 0; i < 8; ++i) {
                        const auto path = MakeTempDirectory();
                        std::lock_guard<std::mutex> lock(mutex);
                        generated.paths.emplace_back(path);
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(mutex);
                    failure = std::current_exception();
                }
            });
        }
        threads.Join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        REQUIRE(generated.paths.size() == 32);
        for (const auto &path : generated.paths) {
            REQUIRE(!path.empty());
            REQUIRE(std::filesystem::is_directory(path));
        }
        REQUIRE(std::set<std::filesystem::path>(generated.paths.begin(), generated.paths.end()).size() == 32);
    });
    return suite.Run(argc, argv);
}
