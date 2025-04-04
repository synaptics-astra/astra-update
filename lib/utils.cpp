// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <string>
#include <iostream>
#include <filesystem>
#include <stdexcept>
#include <iostream>
#include <string>
#include <stdint.h>
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
    if (GetTempPath(MAX_PATH, tempPath) == 0)
    {
        throw std::runtime_error("Failed to get temp path");
    }

    // Generate a unique directory name
    std::stringstream ss;
    ss << tempPath << "TMP" << std::setw(8) << std::setfill('0') << GetTickCount();

    std::string tempDir = ss.str();

    if (!CreateDirectory(tempDir.c_str(), NULL))
    {
        throw std::runtime_error("Failed to create temp directory");
    }

    return tempDir;
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