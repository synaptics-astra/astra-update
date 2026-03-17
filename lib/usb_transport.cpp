// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <iostream>
#include <iomanip>
#include <libusb-1.0/libusb.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <sstream>

#include "usb_transport.hpp"
#include "astra_log.hpp"

USBTransport::~USBTransport() = default;

std::vector<std::string> USBTransport::ParseFilterPortString(const std::string & filterPorts)
{
    ASTRA_LOG;

    std::vector<std::string> filterList;

    if (!filterPorts.empty()) {
        size_t start = 0;
        size_t end = 0;
        while ((end = filterPorts.find(',', start)) != std::string::npos) {
            std::string port = filterPorts.substr(start, end - start);
            if (!port.empty()) {
                filterList.push_back(port);
                log(ASTRA_LOG_LEVEL_DEBUG) << "Adding filter port: " << port << endLog;
            }
            start = end + 1;
        }
        std::string lastPort = filterPorts.substr(start);
        if (!lastPort.empty()) {
            filterList.push_back(lastPort);
            log(ASTRA_LOG_LEVEL_DEBUG) << "Adding filter port: " << lastPort << endLog;
        }
    }

    return filterList;
}