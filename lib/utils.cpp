// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <string>
#include <iostream>
#include <filesystem>
#include <stdexcept>
#include <iostream>
#include <string>
#include <stdint.h>
#include <atomic>
#include <sstream>
#include <iomanip>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#endif

#ifdef PLATFORM_MACOS
#include <libkern/OSByteOrder.h>
#define htole32(x) OSSwapHostToLittleInt32(x)
#endif

#include "utils.hpp"

#if defined(PLATFORM_MACOS) || defined(PLATFORM_LINUX)
#include <unistd.h>

std::string MakeTempDirectory()
{
    char temp[] = "/tmp/astra-update-XXXXXX";
    if (mkdtemp(temp) == nullptr) {
        return "";
    }

    return std::string(temp);
}

uint32_t HostToLE(uint32_t val)
{
#ifdef PLATFORM_MACOS
    return OSSwapHostToLittleInt32(val);
#else
    return htole32(val);
#endif
}
#elif defined(PLATFORM_WINDOWS)
std::string MakeTempDirectory()
{
    char tempPath[MAX_PATH];
    const DWORD tempPathLength = GetTempPathA(MAX_PATH, tempPath);
    if (tempPathLength == 0 || tempPathLength > MAX_PATH)
    {
        // Return empty rather than throwing, matching the POSIX
        // implementation and the caller's contract.
        return "";
    }

    // Name the directory from the process id plus a per-process counter, and
    // retry on collision.  The previous name was GetTempPath() + "TMP" +
    // GetTickCount(), so two instances starting within the same tick chose the
    // same directory and the loser threw -- and flashing several boards at
    // once, which runs several instances concurrently, is a supported
    // workflow.
    static std::atomic<unsigned int> counter{0};
    const DWORD processId = GetCurrentProcessId();

    for (int attempt = 0; attempt < 64; ++attempt)
    {
        std::ostringstream ss;
        ss << tempPath << "astra-update-" << processId << "-" << GetTickCount()
           << "-" << counter.fetch_add(1);

        const std::string tempDir = ss.str();

        if (CreateDirectoryA(tempDir.c_str(), nullptr))
        {
            return tempDir;
        }

        if (GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return "";
        }
    }

    return "";
}

uint32_t HostToLE(uint32_t val)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return val;
#else
    return _byteswap_ulong(val);
#endif
}
#endif