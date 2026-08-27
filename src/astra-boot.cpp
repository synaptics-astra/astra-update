// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <iostream>
#include <memory>
#include <string>
#include <cxxopts.hpp>

#include "cli_common.hpp"
#include "astra_device_manager.hpp"
#include "flash_image.hpp"
#include "astra_device.hpp"

const std::string astraBootVersion = "2.0.3+fable-review";

int main(int argc, char* argv[])
{
    cxxopts::Options options("AstraBoot", "Astra USB Boot Utility");

    astra_cli::InstallSignalHandler();

    options.add_options()
        ("l,log", "Log file path", cxxopts::value<std::string>()->default_value(""))
        ("D,debug", "Enable debug logging", cxxopts::value<bool>()->default_value("false"))
        ("C,continuous", "Enabled updating multiple devices", cxxopts::value<bool>()->default_value("false"))
        ("h,help", "Print usage")
        ("T,temp-dir", "Temporary directory", cxxopts::value<std::string>()->default_value(""))
        ("M,manifest", "Manifest file path", cxxopts::value<std::string>())
        ("u,usb-debug", "Enable USB debug logging", cxxopts::value<bool>()->default_value("false"))
        ("S,simple-progress", "Disable progress bars and report progress messages", cxxopts::value<bool>()->default_value("false"))
        ("o,boot-command", "Boot command", cxxopts::value<std::string>()->default_value(""))
        ("boot-image", "Boot Image Path", cxxopts::value<std::string>())
        ("p,port", "Filter based on USB port", cxxopts::value<std::string>()->default_value(""))
        ("e,exit-on-error", "Exit if an error occurs when running in continuous mode", cxxopts::value<bool>()->default_value("false"))
        ("b,boot-stage", "Target boot stage: auto, bootloader, linux, m52bl, sysmgr", cxxopts::value<std::string>()->default_value("auto"))
        ("v,version", "Print version");

    options.parse_positional({"boot-image"});

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
        std::cout << "astra-boot: v" << astraBootVersion <<
            " (lib v" << AstraDeviceManager::GetVersion() << ")" << std::endl;
        return 0;
    }

    if (!result.count("boot-image")) {
        std::cerr << "Error: Missing required positional argument: boot-image" << std::endl;
        std::cout << options.help() << std::endl;
        return -1;
    }

    std::string bootImagePath = result["boot-image"].as<std::string>();
    std::string logFilePath = result["log"].as<std::string>();
    std::string tempDir = result["temp-dir"].as<std::string>();
    bool debug = result["debug"].as<bool>();
    bool continuous = result["continuous"].as<bool>();
    bool exitOnError = result["exit-on-error"].as<bool>();
    AstraLogLevel logLevel = debug ?  ASTRA_LOG_LEVEL_DEBUG : ASTRA_LOG_LEVEL_INFO;
    bool usbDebug = result["usb-debug"].as<bool>();
    bool simpleProgress = result["simple-progress"].as<bool>();
    std::string bootCommand = result["boot-command"].as<std::string>();
    std::string filterPorts = result["port"].as<std::string>();
    std::string bootStageStr = result["boot-stage"].as<std::string>();

    AstraDeviceBootStage bootStage = AstraDevice::BootStageFromString(bootStageStr);

    if (usbDebug) {
        // Use simple progress when USB debugging is enabled
        // because libusb will output to stdout and conflict with progress bars
        simpleProgress = true;
    }

    std::string manifest = "";
    if (result.count("manifest")) {
        manifest = result["manifest"].as<std::string>();
    }

    // DynamicProgress to manage multiple progress bars
    indicators::DynamicProgress<indicators::ProgressBar> dynamicProgress;
    astra_cli::ProgressBars progressBars;

    dynamicProgress.set_option(indicators::option::HideBarWhenComplete{false});

    std::cout << "Astra Boot\n" << std::endl;

    AstraDeviceManager deviceManager(astra_cli::ResponseCallback, continuous, logLevel, logFilePath, tempDir, filterPorts, usbDebug);

    try {
        deviceManager.Boot(bootImagePath, bootCommand, bootStage);
     } catch (const std::exception& e) {
        std::cerr << "Failed to initialize boot: " << e.what() << std::endl;
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
            } else if (deviceResponse.m_status == ASTRA_DEVICE_STATUS_BOOT_FAIL) {
                std::cout << "Device: " << deviceResponse.m_deviceName << " Boot Failed: "
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
