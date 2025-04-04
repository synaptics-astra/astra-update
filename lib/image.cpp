// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <filesystem>
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

    uint32_t size = std::filesystem::file_size(m_imagePath);
    log(ASTRA_LOG_LEVEL_DEBUG) << "Image size: " << size << endLog;

    if (m_fp) {
        fclose(m_fp);
    }

    FILE *fp = fopen(m_imagePath.c_str(), "rb");
    if (fp == nullptr) {
        log(ASTRA_LOG_LEVEL_ERROR) << "Failed to open file: " << m_imagePath << endLog;
        log(ASTRA_LOG_LEVEL_ERROR) << strerror(errno) << endLog;
        return -1;
    }

    m_imageSize = size;
    m_fp = fp;

    return 0;
}

int Image::GetDataBlock(uint8_t *data, size_t size)
{
    ASTRA_LOG;

    int readSize = size;
    if (m_imageSize < size) {
        readSize = m_imageSize;
    }

    long currentPos = ftell(m_fp);
    if (currentPos + static_cast<long>(readSize) > m_imageSize) {
        readSize = m_imageSize - currentPos;
    }

    size_t bytesRead = fread(data, 1, readSize, m_fp);
    if (bytesRead != readSize) {
        return -1;
    }

    return readSize;
}

Image::~Image()
{
    ASTRA_LOG;

    if (m_fp) {
        fclose(m_fp);
    }
}