// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "fastboot_device.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "astra_log.hpp"

FastBootDevice::FastBootDevice(USBDevice *usbDevice)
    : m_usbDevice(usbDevice)
{}

FastBootDevice::~FastBootDevice()
{
    Close();
}

bool FastBootDevice::Open(std::function<void()> disconnectCallback)
{
    ASTRA_LOG;

    if (m_opened) {
        return true;
    }

    m_disconnectCallback = disconnectCallback;
    m_disconnected = false;

    int ret = m_usbDevice->Open(
        std::bind(&FastBootDevice::USBEventHandler, this,
            std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    if (ret < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: failed to open USB device" << endLog;
        return false;
    }

    m_opened = true;
    log(ASTRA_LOG_LEVEL_DEBUG) << "FastBootDevice: opened" << endLog;
    return true;
}

void FastBootDevice::Close()
{
    ASTRA_LOG;

    if (!m_opened) {
        return;
    }

    m_opened = false;
    // The USBDevice itself is closed by the caller / AstraDeviceImpl.
    log(ASTRA_LOG_LEVEL_DEBUG) << "FastBootDevice: closed" << endLog;
}

void FastBootDevice::USBEventHandler(USBDevice::USBEvent event, uint8_t * /*buf*/, size_t /*size*/)
{
    if (event == USBDevice::USB_DEVICE_EVENT_NO_DEVICE ||
        event == USBDevice::USB_DEVICE_EVENT_TRANSFER_CANCELED ||
        event == USBDevice::USB_DEVICE_EVENT_TRANSFER_ERROR)
    {
        ASTRA_LOG;
        log(ASTRA_LOG_LEVEL_DEBUG) << "FastBootDevice: USB device disconnected" << endLog;
        m_disconnected = true;
        if (m_disconnectCallback) {
            m_disconnectCallback();
        }
    }
}

bool FastBootDevice::SendCommand(const std::string &command)
{
    ASTRA_LOG;

    if (command.size() > kCmdBufferSize) {
        log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: command too long: " << command.size() << endLog;
        return false;
    }

    // Cast away const for Write — data is not modified
    auto *data = reinterpret_cast<uint8_t *>(const_cast<char *>(command.c_str()));
    int transferred = 0;
    int ret = m_usbDevice->Write(data, command.size(), &transferred);
    if (ret < 0) {
        log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: send command failed: " << command << endLog;
        return false;
    }

    log(ASTRA_LOG_LEVEL_DEBUG) << "FastBootDevice: sent command: " << command << endLog;
    return true;
}

bool FastBootDevice::ReadResponse(std::string &status, std::string &message, int timeoutMs)
{
    ASTRA_LOG;

    uint8_t buf[kRespBufferSize + 1] = {};
    int received = 0;

    int ret = m_usbDevice->ReadBulk(buf, kRespBufferSize, &received, timeoutMs);
    if (ret < 0) {
        // Log at DEBUG: the device may have reset before sending a response
        // (e.g. after fb_exit triggers a U-Boot reboot).  Callers log the
        // consequence at the appropriate level.
        log(ASTRA_LOG_LEVEL_DEBUG) << "FastBootDevice: read response failed (ret=" << ret << ")" << endLog;
        return false;
    }

    if (received < 4) {
        log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: response too short: " << received << " bytes" << endLog;
        return false;
    }

    buf[received] = '\0';
    status  = std::string(reinterpret_cast<char *>(buf), 4);
    message = std::string(reinterpret_cast<char *>(buf + 4), static_cast<size_t>(received - 4));

    log(ASTRA_LOG_LEVEL_DEBUG) << "FastBootDevice: response status='" << status
        << "' message='" << message << "'" << endLog;
    return true;
}

std::string FastBootDevice::ExecuteCommand(const std::string &command, uint32_t &dataSize,
    int timeoutMs)
{
    ASTRA_LOG;

    if (!SendCommand(command)) {
        return "";
    }

    dataSize = 0;

    for (;;) {
        std::string status;
        std::string message;
        if (!ReadResponse(status, message, timeoutMs)) {
            return "";
        }

        if (status == "INFO") {
            log(ASTRA_LOG_LEVEL_INFO) << "FastBootDevice INFO: " << message << endLog;
            continue;
        }

        if (status == "DATA") {
            // message is 8-digit hex size
            try {
                dataSize = static_cast<uint32_t>(std::stoul(message, nullptr, 16));
            } catch (const std::exception &) {
                log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: invalid DATA size: " << message << endLog;
                return "";
            }
        }

        if (status == "FAIL") {
            log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: FAIL: " << message << endLog;
        }

        return status;
    }
}

bool FastBootDevice::ProbeSerial(USBDevice *device, std::string &out)
{
    ASTRA_LOG;

    // Open the device with a no-op callback.  LibUSBDevice::Open() will
    // replace this callback with the real one when FastBootDevice::Open()
    // is called later by the normal boot path.
    if (device->Open([](USBDevice::USBEvent, uint8_t *, size_t) {}) < 0) {
        log(ASTRA_LOG_LEVEL_DEBUG) << "ProbeSerial: failed to open USB device" << endLog;
        return false;
    }

    // Use a temporary FastBootDevice purely for protocol framing.
    // We bypass FastBootDevice::Open() deliberately so its dtor does not
    // try to close a device it never opened (m_opened stays false).
    FastBootDevice fb(device);
    bool ok = fb.GetVar("serialno", out);
    log(ASTRA_LOG_LEVEL_DEBUG) << "ProbeSerial: serialno='" << out << "'" << endLog;
    return ok;
}

bool FastBootDevice::GetVar(const std::string &name, std::string &value, int timeoutMs)
{
    ASTRA_LOG;

    if (!SendCommand("getvar:" + name)) {
        return false;
    }

    for (;;) {
        std::string status;
        std::string message;
        if (!ReadResponse(status, message, timeoutMs)) {
            return false;
        }

        if (status == "INFO") {
            log(ASTRA_LOG_LEVEL_INFO) << "FastBootDevice INFO: " << message << endLog;
            continue;
        }

        if (status == "OKAY") {
            value = message;
            log(ASTRA_LOG_LEVEL_DEBUG) << "FastBootDevice: getvar " << name << " = '" << value << "'" << endLog;
            return true;
        }

        log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: getvar " << name << " failed: " << status
            << " " << message << endLog;
        return false;
    }
}

bool FastBootDevice::StageFile(const std::string &path,
    std::function<void(size_t, size_t)> progressCb, int timeoutMs)
{
    ASTRA_LOG;

    // Determine file size
    std::error_code ec;
    const uintmax_t fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
        log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: cannot stat file: " << path
            << " (" << ec.message() << ")" << endLog;
        return false;
    }

    // Send download:<size> command
    std::ostringstream cmdStream;
    cmdStream << "download:" << std::setw(8) << std::setfill('0') << std::hex << fileSize;
    const std::string downloadCmd = cmdStream.str();

    uint32_t acceptedSize = 0;
    const std::string result = ExecuteCommand(downloadCmd, acceptedSize, timeoutMs);
    if (result != "DATA") {
        log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: download command failed (result=" << result << ")" << endLog;
        return false;
    }

    log(ASTRA_LOG_LEVEL_DEBUG) << "FastBootDevice: device accepted download of " << acceptedSize << " bytes" << endLog;

    // Stream the file in chunks
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: cannot open file: " << path << endLog;
        return false;
    }

    std::vector<uint8_t> chunk(kDownloadChunkSize);
    size_t totalSent = 0;
    const size_t total = static_cast<size_t>(fileSize);

    while (totalSent < total) {
        const size_t toRead = std::min(kDownloadChunkSize, total - totalSent);
        file.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(toRead));
        const auto bytesRead = static_cast<size_t>(file.gcount());
        if (bytesRead == 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: premature end of file: " << path << endLog;
            return false;
        }

        int transferred = 0;
        const int ret = m_usbDevice->Write(chunk.data(), bytesRead, &transferred);
        if (ret < 0) {
            log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: file data write failed" << endLog;
            return false;
        }

        totalSent += static_cast<size_t>(transferred);
        if (progressCb) {
            progressCb(totalSent, total);
        }
    }

    // Wait for final OKAY
    std::string status;
    std::string message;
    // Use a generous timeout for the device to acknowledge the data
    if (!ReadResponse(status, message, timeoutMs)) {
        log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: no response after data transfer" << endLog;
        return false;
    }

    if (status != "OKAY") {
        log(ASTRA_LOG_LEVEL_ERROR) << "FastBootDevice: download not acknowledged: " << status
            << " " << message << endLog;
        return false;
    }

    log(ASTRA_LOG_LEVEL_DEBUG) << "FastBootDevice: stage complete for " << path
        << " (" << totalSent << " bytes)" << endLog;
    return true;
}

bool FastBootDevice::Oem(const std::string &command, int timeoutMs)
{
    ASTRA_LOG;

    const std::string fullCmd = "oem " + command;
    uint32_t unused = 0;
    const std::string result = ExecuteCommand(fullCmd, unused, timeoutMs);
    if (result == "OKAY") {
        return true;
    }

    log(ASTRA_LOG_LEVEL_WARNING) << "FastBootDevice: oem command failed: " << command
        << " (result=" << result << ")" << endLog;
    return false;
}

bool FastBootDevice::OemNoWait(const std::string &command)
{
    ASTRA_LOG;

    const std::string fullCmd = "oem " + command;
    const bool ok = SendCommand(fullCmd);
    if (!ok) {
        log(ASTRA_LOG_LEVEL_WARNING) << "FastBootDevice: oem (no-wait) send failed: " << command << endLog;
    }
    return ok;
}
