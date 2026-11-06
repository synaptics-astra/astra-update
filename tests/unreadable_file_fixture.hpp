// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#pragma once

#include "test_support.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace test {

class UnreadableFile
{
public:
    explicit UnreadableFile(const std::filesystem::path &path) : m_path(path)
    {
#if defined(_WIN32)
        m_handle = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to lock test file: " + std::to_string(GetLastError()));
        }
#else
        m_permissions = std::filesystem::status(path).permissions();
        std::filesystem::permissions(path, std::filesystem::perms::none);
#endif
    }

    ~UnreadableFile()
    {
#if defined(_WIN32)
        if (!CloseHandle(m_handle)) {
            ++cleanupFailures;
            std::cerr << "Failed to release test file lock\n";
        }
#else
        std::error_code error;
        std::filesystem::permissions(m_path, m_permissions, error);
        if (error) {
            ++cleanupFailures;
            std::cerr << "Failed to restore file permissions: " << error.message() << '\n';
        }
#endif
    }

    UnreadableFile(const UnreadableFile &) = delete;
    UnreadableFile &operator=(const UnreadableFile &) = delete;

    void RequireUnreadable() const
    {
        std::ifstream probe(m_path, std::ios::binary);
#if !defined(_WIN32)
        if (probe.is_open()) {
            throw Skip("This process can read permission-restricted files");
        }
#endif
        REQUIRE(!probe.is_open());
    }

private:
    std::filesystem::path m_path;
#if defined(_WIN32)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    std::filesystem::perms m_permissions;
#endif
};

} // namespace test
