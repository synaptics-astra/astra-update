// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <algorithm>
#include <filesystem>
#include <limits>
#include <vector>
#include <iostream>
#include <cstring>

#include "image.hpp"
#include "astra_log.hpp"

int Image::Load()
{
    ASTRA_LOG;

    log(ASTRA_LOG_LEVEL_DEBUG) << "Loading image: " << m_imagePath << endLog;
    m_imageName = std::filesystem::path(m_imagePath).filename().string();

    if (std::filesystem::exists(m_imagePath) == false) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Image file does not exist: " << m_imagePath << endLog;
        return -1;
    }

    // Use the error_code overload: the throwing form would propagate out of
    // the image-request thread if the file disappears after the check above.
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(m_imagePath, ec);
    if (ec) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to query image size for " << m_imagePath
            << ": " << ec.message() << endLog;
        return -1;
    }

    if (size > std::numeric_limits<size_t>::max()) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Image file too large: " << m_imagePath << endLog;
        return -1;
    }

    log(ASTRA_LOG_LEVEL_DEBUG) << "Image size: " << size << endLog;

    FILE *fp = fopen(m_imagePath.c_str(), "rb");
    if (fp == nullptr) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open file: " << m_imagePath << endLog;
        log(ASTRA_LOG_LEVEL_ERROR) << strerror(errno) << endLog;
        return -1;
    }

    m_imageSize = static_cast<size_t>(size);
    // Assigning releases any previously held handle, closing it if this was
    // the last reference.
    m_fp = std::shared_ptr<FILE>(fp, FileCloser{});

    return 0;
}

int Image::GetDataBlock(uint8_t *data, size_t size)
{
    ASTRA_LOG;

    FILE *fp = m_fp.get();
    if (fp == nullptr) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Image not loaded: " << m_imagePath << endLog;
        return -1;
    }

    // Clamp with unsigned arithmetic.  The previous version mixed a signed
    // ftell result with the unsigned size in the comparison and truncated
    // size_t to int, which misbehaved on large images and on a failed ftell.
    const long currentPos = ftell(fp);
    if (currentPos < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to query position in " << m_imagePath << endLog;
        return -1;
    }

    const size_t position = static_cast<size_t>(currentPos);
    if (position >= m_imageSize) {
        return 0;
    }

    const size_t readSize = std::min(size, m_imageSize - position);

    const size_t bytesRead = fread(data, 1, readSize, fp);
    if (bytesRead != readSize) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Short read from " << m_imagePath << ": expected "
            << readSize << " bytes, got " << bytesRead << endLog;
        return -1;
    }

    // readSize is bounded by the caller's buffer size, so this fits in an int.
    return static_cast<int>(bytesRead);
}