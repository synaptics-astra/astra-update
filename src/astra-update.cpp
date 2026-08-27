// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <cxxopts.hpp>

#include "cli_common.hpp"
#include "astra_device_manager.hpp"
#include "flash_image.hpp"
#include "astra_device.hpp"

const std::string astraUpdateVersion = "2.0.3+fable-review";

int main(int argc, char* argv[])
{
    cxxopts::Options options("AstraUpdate", "Astra Update Utility");

    astra_cli::InstallSignalHandler();

    options.add_options()
        ("B,boot-image-collection", "Astra Boot Image path (default: $ASTRA_USBBOOT_IMAGES or astra-usbboot-images)", cxxopts::value<std::string>())
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
        ("d,ddr-type", "DDR type", cxxopts::value<std::string>()->default_value("not_specified"))
        ("u,usb-debug", "Enable USB debug logging", cxxopts::value<bool>()->default_value("false"))
        ("S,simple-progress", "Disable progress bars and report progress messages", cxxopts::value<bool>()->default_value("false"))
        ("p,port", "Filter based on USB port", cxxopts::value<std::string>()->default_value(""))
        ("r,disable-reset", "Reset the device after a successful update", cxxopts::value<bool>()->default_value("false"))
        ("e,exit-on-error", "Exit if an error occurs when running in continuous mode", cxxopts::value<bool>()->default_value("false"))
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
    std::string bootImagesPath;
    if (result.count("boot-image-collection")) {
        bootImagesPath = result["boot-image-collection"].as<std::string>();
    } else {
        const char *envPath = std::getenv("ASTRA_USBBOOT_IMAGES");
        bootImagesPath = (envPath != nullptr && envPath[0] != '\0') ? envPath : "astra-usbboot-images";
    }
    std::string logFilePath = result["log"].as<std::string>();
    std::string tempDir = result["temp-dir"].as<std::string>();
    bool debug = result["debug"].as<bool>();
    bool continuous = result["continuous"].as<bool>();
    bool exitOnError = result["exit-on-error"].as<bool>();
    AstraLogLevel logLevel = debug ?  ASTRA_LOG_LEVEL_DEBUG : ASTRA_LOG_LEVEL_INFO;
    bool usbDebug = result["usb-debug"].as<bool>();
    bool simpleProgress = result["simple-progress"].as<bool>();
    std::string filterPorts = result["port"].as<std::string>();

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
    if (result.count("ddr-type")) {
        config["ddr_type"] = result["ddr-type"].as<std::string>();
    }
    if (result.count("disable-reset")) {
        config["reset"] = result["disable-reset"].as<bool>() ? "disable" : "enable";
    }

    // DynamicProgress to manage multiple progress bars
    indicators::DynamicProgress<indicators::ProgressBar> dynamicProgress;
    astra_cli::ProgressBars progressBars;

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
        std::cerr << "Failed to load flash image";
        if (!flashImage->GetLoadError().empty()) {
            std::cerr << ": " << flashImage->GetLoadError();
        }
        std::cerr << std::endl;
        return -1;
    }

    std::cout << "Update Image: " << flashImage->GetChipName() << " " << flashImage->GetBoardName() << std::endl;
    std::cout << "    Image Type: " << AstraFlashImageTypeToString(flashImage->GetFlashImageType()) << std::endl;
    std::cout << "    Secure Boot: " << AstraSecureBootVersionToString(flashImage->GetSecureBootVersion()) << std::endl;
    std::cout << "    Memory Layout: " << AstraMemoryLayoutToString(flashImage->GetMemoryLayout()) << std::endl;
    std::cout << "    DDR Type: " << AstraMemoryDDRTypeToString(flashImage->GetMemoryDDRType()) << std::endl;
    std::cout << "    Boot Image ID: " << flashImage->GetBootImageId() << "\n" << std::endl;

    AstraDeviceManager deviceManager(astra_cli::ResponseCallback, continuous, logLevel, logFilePath, tempDir, filterPorts, usbDebug);

    try {
        deviceManager.Update(flashImage, bootImagesPath);
     } catch (const std::exception& e) {
        std::cerr << "Failed to initialize update: " << e.what() << std::endl;
        return -1;
     }

    indicators::show_console_cursor(false);

    while (astra_cli::g_running.load()) {
        // WaitForResponse() pops the response and releases the queue lock
        // before returning, so the rendering below cannot stall the device
        // threads on terminal I/O.
        auto response = astra_cli::WaitForResponse();
        if (!response) {
            continue; // timed out, or shutting down: the loop condition decides
        }

        if (response->IsDeviceManagerResponse()) {
            const auto &managerResponse = response->GetDeviceManagerResponse();

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
        } else if (response->IsDeviceResponse()) {
            const auto &deviceResponse = response->GetDeviceResponse();

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
                std::cout << "Device: " << deviceResponse.m_deviceName << " Boot Failed: "
                          << deviceResponse.m_message << std::endl;
                if (continuous && exitOnError) {
                    astra_cli::g_running.store(false);
                    break;
                }
            } else if (deviceResponse.m_status == ASTRA_DEVICE_STATUS_UPDATE_FAIL) {
                std::cout << "Device: " << deviceResponse.m_deviceName << " Update Failed: "
                          << deviceResponse.m_message << std::endl;
                if (continuous && exitOnError) {
                    astra_cli::g_running.store(false);
                    break;
                }
            } else if (deviceResponse.m_status == ASTRA_DEVICE_STATUS_IMAGE_SEND_START ||
                deviceResponse.m_status == ASTRA_DEVICE_STATUS_IMAGE_SEND_PROGRESS ||
                deviceResponse.m_status == ASTRA_DEVICE_STATUS_IMAGE_SEND_COMPLETE)
            {
                astra_cli::ReportImageProgress(deviceResponse, simpleProgress,
                    dynamicProgress, progressBars);
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
