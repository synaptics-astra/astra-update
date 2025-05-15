// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <iostream>
#include <memory>
#include <queue>
#include <condition_variable>
#include <functional>
#include <cxxopts.hpp>
#include <indicators/progress_bar.hpp>
#include <indicators/dynamic_progress.hpp>
#include <unordered_map>
#include <csignal>

#include "astra_device_manager.hpp"
#include "flash_image.hpp"
#include "astra_device.hpp"

const std::string astraUpdateVersion = "1.0.1";

// Define a struct to hold the two strings
struct DeviceImageKey {
    std::string deviceName;
    std::string imageName;

    bool operator==(const DeviceImageKey& other) const {
        return deviceName == other.deviceName && imageName == other.imageName;
    }
};

// Implement a custom hash function for the struct
struct DeviceImageKeyHash {
    std::size_t operator()(const DeviceImageKey& key) const {
        return std::hash<std::string>()(key.deviceName) ^ std::hash<std::string>()(key.imageName);
    }
};

std::queue<AstraDeviceManagerResponse> managerResponses;
std::condition_variable managerResponsesCV;
std::mutex managerResponsesMutex;
std::atomic<bool> running{true};

void AstraDeviceManagerResponseCallback(AstraDeviceManagerResponse response)
{
    std::lock_guard<std::mutex> lock(managerResponsesMutex);
    managerResponses.push(response);
    managerResponsesCV.notify_one();
}

void UpdateProgressBars(DeviceResponse &deviceResponse,
    indicators::DynamicProgress<indicators::ProgressBar> &dynamicProgress,
    std::unordered_map<DeviceImageKey, size_t, DeviceImageKeyHash> &progressBars)
{
    DeviceImageKey key{deviceResponse.m_deviceName, deviceResponse.m_imageName};

    // Ensure a progress bar exists for this image
    if (progressBars.find(key) == progressBars.end()) {
        auto progress_bar = std::make_unique<indicators::ProgressBar>(
            indicators::option::BarWidth{50},
            indicators::option::Start{"["},
            indicators::option::Fill{"="},
            indicators::option::Lead{">"},
            indicators::option::Remainder{" "},
            indicators::option::End{"]"},
            indicators::option::PostfixText{deviceResponse.m_imageName},
            indicators::option::PrefixText{deviceResponse.m_deviceName + ": "},
            indicators::option::ForegroundColor{indicators::Color::green},
            indicators::option::ShowElapsedTime{true},
            indicators::option::ShowRemainingTime{true},
            indicators::option::MaxProgress{100}
        );
        size_t bardId = dynamicProgress.push_back(std::move(progress_bar));
        progressBars[key] = bardId;
    }

    auto& progressBar = dynamicProgress[progressBars[key]];
    progressBar.set_progress(deviceResponse.m_progress);

    if (deviceResponse.m_progress == 100) {
        progressBar.mark_as_completed();
    }
}

void UpdateSimpleProgress(DeviceResponse &deviceResponse)
{
    std::cout << "Device: " << deviceResponse.m_deviceName
                << " Image: " << deviceResponse.m_imageName
                << " Progress: " << deviceResponse.m_progress << std::endl;
}

void SignalHandler(int signal)
{
    if (signal == SIGINT) {
        running.store(false);
        managerResponsesCV.notify_all();
    }
}

int main(int argc, char* argv[])
{
    cxxopts::Options options("AstraUpdate", "Astra Update Utility");

    std::signal(SIGINT, SignalHandler);

    options.add_options()
        ("B,boot-image-collection", "Astra Boot Image path", cxxopts::value<std::string>()->default_value("astra-usbboot-images"))
        ("l,log", "Log file path", cxxopts::value<std::string>()->default_value(""))
        ("D,debug", "Enable debug logging", cxxopts::value<bool>()->default_value("false"))
        ("C,continuous", "Enabled updating multiple devices", cxxopts::value<bool>()->default_value("false"))
        ("h,help", "Print usage")
        ("T,temp-dir", "Temporary directory", cxxopts::value<std::string>()->default_value(""))
        ("f,flash", "Flash image path", cxxopts::value<std::string>()->default_value("eMMCimg"))
        ("b,board", "Board name", cxxopts::value<std::string>())
        ("c,chip", "Chip name", cxxopts::value<std::string>())
        ("M,manifest", "Manifest file path", cxxopts::value<std::string>())
        ("i,boot-image-id", "Boot bootImages ID", cxxopts::value<std::string>())
        ("t,image-type", "Image type", cxxopts::value<std::string>())
        ("s,secure-boot", "Secure boot version", cxxopts::value<std::string>()->default_value("genx"))
        ("m,memory-layout", "Memory layout", cxxopts::value<std::string>())
        ("u,usb-debug", "Enable USB debug logging", cxxopts::value<bool>()->default_value("false"))
        ("S,simple-progress", "Disable progress bars and report progress messages", cxxopts::value<bool>()->default_value("false"))
        ("v,version", "Print version");

    cxxopts::ParseResult result;
    try {
        result = options.parse(argc, argv);
    } catch (const cxxopts::OptionException& e) {
        std::cerr << "Error parsing options: " << e.what() << std::endl;
        std::cerr << options.help() << std::endl;
        return -1;
    }

    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    if (result.count("version")) {
        std::cout << "astra-update: v" << astraUpdateVersion <<
            " (lib v" << AstraDeviceManager::GetVersion() << ")" << std::endl;
        return 0;
    }

    std::string flashImagePath = result["flash"].as<std::string>();
    std::string bootImagesPath = result["boot-image-collection"].as<std::string>();
    std::string logFilePath = result["log"].as<std::string>();
    std::string tempDir = result["temp-dir"].as<std::string>();
    bool debug = result["debug"].as<bool>();
    bool continuous = result["continuous"].as<bool>();
    AstraLogLevel logLevel = debug ?  ASTRA_LOG_LEVEL_DEBUG : ASTRA_LOG_LEVEL_INFO;
    bool usbDebug = result["usb-debug"].as<bool>();
    bool simpleProgress = result["simple-progress"].as<bool>();

    if (usbDebug) {
        // Use simple progress when USB debugging is enabled
        // because libusb will output to stdout and conflict with progress bars
        simpleProgress = true;
    }

    std::string manifest = "";
    if (result.count("manifest")) {
        manifest = result["manifest"].as<std::string>();
    }

    std::map<std::string, std::string> config;
    if (result.count("board")) {
        config["board"] = result["board"].as<std::string>();
    }
    if (result.count("chip")) {
        config["chip"] = result["chip"].as<std::string>();
    }
    if (result.count("image-type")) {
        config["image_type"] = result["image-type"].as<std::string>();
    }
    if (result.count("boot-image-id")) {
        config["boot_image"] = result["boot-image-id"].as<std::string>();
    }
    if (result.count("secure-boot")) {
        config["secure_boot"] = result["secure-boot"].as<std::string>();
    }
    if (result.count("memory-layout")) {
        config["memory_layout"] = result["memory-layout"].as<std::string>();
    }

    // DynamicProgress to manage multiple progress bars
    indicators::DynamicProgress<indicators::ProgressBar> dynamicProgress;
    std::unordered_map<DeviceImageKey, size_t, DeviceImageKeyHash> progressBars;

    dynamicProgress.set_option(indicators::option::HideBarWhenComplete{false});

    std::cout << "Astra Update\n" << std::endl;

    std::shared_ptr<FlashImage> flashImage;
    try {
        flashImage = FlashImage::FlashImageFactory(flashImagePath, config, manifest);
    } catch (const std::exception& e) {
        std::cerr << "Failed to load flash image: " << e.what() << std::endl;
        return -1;
    }

    int ret = flashImage->Load();
    if (ret < 0) {
        std::cerr << "Failed to load flash image" << std::endl;
        return -1;
    }

    std::cout << "Update Image: " << flashImage->GetChipName() << " " << flashImage->GetBoardName() << std::endl;
    std::cout << "    Image Type: " << AstraFlashImageTypeToString(flashImage->GetFlashImageType()) << std::endl;
    std::cout << "    Secure Boot: " << AstraSecureBootVersionToString(flashImage->GetSecureBootVersion()) << std::endl;
    std::cout << "    Memory Layout: " << AstraMemoryLayoutToString(flashImage->GetMemoryLayout()) << std::endl;
    std::cout << "    Boot Image ID: " << flashImage->GetBootImageId() << "\n" << std::endl;

    AstraDeviceManager deviceManager(AstraDeviceManagerResponseCallback, continuous, logLevel, logFilePath, tempDir, usbDebug);

    try {
        deviceManager.Update(flashImage, bootImagesPath);
     } catch (const std::exception& e) {
        std::cerr << "Failed to initialize update: " << e.what() << std::endl;
        return -1;
     }

    indicators::show_console_cursor(false);

    if (running.load()) {
        while (true) {
            std::unique_lock<std::mutex> lock(managerResponsesMutex);
            managerResponsesCV.wait(lock, []{ return !managerResponses.empty() || !running.load(); });

            if (!running.load()) {
                break;
            }

            auto status = managerResponses.front();
            managerResponses.pop();

            if (status.IsDeviceManagerResponse()) {
                auto managerResponse = status.GetDeviceManagerResponse();
                if (managerResponse.m_managerStatus == ASTRA_DEVICE_MANAGER_STATUS_INFO) {
                    std::cout << managerResponse.m_managerMessage << "\n" << std::endl;
                } else if (managerResponse.m_managerStatus == ASTRA_DEVICE_MANAGER_STATUS_SHUTDOWN) {
                    break;
                } else if (managerResponse.m_managerStatus == ASTRA_DEVICE_MANAGER_STATUS_START) {
                    std::cout << managerResponse.m_managerMessage << "\n" << std::endl;
                } else {
                    std::cout << "Device Manager status: " << managerResponse.m_managerStatus
                            << " Message: " << managerResponse.m_managerMessage << std::endl;
                }
            } else if (status.IsDeviceResponse()) {
                auto deviceResponse = status.GetDeviceResponse();

                if (deviceResponse.m_status == ASTRA_DEVICE_STATUS_ADDED) {
                    std::cout << "Detected Device: " << deviceResponse.m_deviceName << std::endl;
                } else if (deviceResponse.m_status == ASTRA_DEVICE_STATUS_BOOT_START) {
                    std::cout << "Booting Device: " << deviceResponse.m_deviceName << std::endl;
                } else if (deviceResponse.m_status == ASTRA_DEVICE_STATUS_BOOT_COMPLETE) {
                    std::cout << "Booting " << deviceResponse.m_deviceName << " is complete" << std::endl;
                } else if (deviceResponse.m_status == ASTRA_DEVICE_STATUS_UPDATE_START) {
                    std::cout << "Updating Device: " << deviceResponse.m_deviceName << std::endl;
                } else if (deviceResponse.m_status == ASTRA_DEVICE_STATUS_UPDATE_COMPLETE) {
                    std::cout << "Device: " << deviceResponse.m_deviceName << " Update Complete" << std::endl;
                } else if (deviceResponse.m_status == ASTRA_DEVICE_STATUS_BOOT_FAIL) {
                    std::cout << "Device: " << deviceResponse.m_deviceName << " Boot Failed: " << deviceResponse.m_message << std::endl;
                } else if (deviceResponse.m_status == ASTRA_DEVICE_STATUS_UPDATE_FAIL) {
                    std::cout << "Device: " << deviceResponse.m_deviceName << " Update Failed: " << deviceResponse.m_message << std::endl;
                } else if (deviceResponse.m_status == ASTRA_DEVICE_STATUS_IMAGE_SEND_START ||
                    deviceResponse.m_status == ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS ||
                    deviceResponse.m_status == ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE)
                {
                    if (simpleProgress) {
                        UpdateSimpleProgress(deviceResponse);
                    } else {
                        UpdateProgressBars(deviceResponse, dynamicProgress, progressBars);
                    }
                }
            }
        }
    }
    indicators::show_console_cursor(true);

    if (deviceManager.Shutdown()) {
        std::cerr << "Error reported: please check the log file for more information: " << deviceManager.GetLogFile() << std::endl;
        return -1;
    }

    return 0;
}
