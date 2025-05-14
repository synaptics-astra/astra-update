// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <string>
#include <memory>
#include <functional>
#include <variant>

#include "astra_device.hpp"
#include "flash_image.hpp"
#include "astra_log.hpp"

#define ASTRA_DEVICE_MANAGER_VERSION "1.0.1"

enum AstraDeviceManagerStatus {
    ASTRA_DEVICE_MANAGER_STATUS_START,
    ASTRA_DEVICE_MANAGER_STATUS_INFO,
    ASTRA_DEVICE_MANAGER_STATUS_FAILURE,
    ASTRA_DEVICE_MANAGER_STATUS_SHUTDOWN,
};

enum AstraDeviceManangerMode {
    ASTRA_DEVICE_MANAGER_MODE_BOOT,
    ASTRA_DEVICE_MANAGER_MODE_UPDATE,
};

class AstraDeviceManager {
public:
    AstraDeviceManager(std::function<void(AstraDeviceManagerResponse)> responseCallback,
        bool updateContinuously = false,
        AstraLogLevel minLogLevel = ASTRA_LOG_LEVEL_WARNING,
        const std::string &logPath = "",
        const std::string &tempDir = "",
        const std::string &filterPorts = "",
        bool usbDebug = false
    );
    ~AstraDeviceManager();

    void Update(std::shared_ptr<FlashImage> flashImage, std::string bootImagePath);
    void Boot(std::string bootImagesPath, std::string bootCommand = "");
    bool Shutdown();
    std::string GetLogFile() const;

    static std::string GetVersion() {
        return ASTRA_DEVICE_MANAGER_VERSION;
    }

private:
    class AstraDeviceManagerImpl;
    std::unique_ptr<AstraDeviceManagerImpl> pImpl;
};

struct ManagerResponse {
    AstraDeviceManagerStatus m_managerStatus;
    std::string m_managerMessage;
};

class AstraDeviceManagerResponse {
public:
    using ResponseVariant = std::variant<ManagerResponse, DeviceResponse>;

    AstraDeviceManagerResponse(ManagerResponse managerResponse)
        : response(managerResponse) {}

        AstraDeviceManagerResponse(DeviceResponse deviceResponse)
        : response(deviceResponse) {}

    bool IsDeviceManagerResponse() const {
        return std::holds_alternative<ManagerResponse>(response);
    }

    bool IsDeviceResponse() const {
        return std::holds_alternative<DeviceResponse>(response);
    }

    const ManagerResponse& GetDeviceManagerResponse() const {
        return std::get<ManagerResponse>(response);
    }

    const DeviceResponse& GetDeviceResponse() const {
        return std::get<DeviceResponse>(response);
    }

private:
    ResponseVariant response;
};