// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

// Infrastructure shared by the astra-update and astra-boot front ends:
// the response queue fed by the device manager, SIGINT handling, and the
// progress reporting.  Both tools previously carried identical copies of
// all of this.
//
// Each tool keeps its own response-handling switch, because the statuses
// they care about differ (astra-boot has no update phase).

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>

#include <indicators/dynamic_progress.hpp>
#include <indicators/progress_bar.hpp>

#include "astra_device_manager.hpp"

namespace astra_cli {

// Identifies one progress bar: one image being sent to one device.
struct DeviceImageKey {
    std::string deviceName;
    std::string imageName;

    bool operator==(const DeviceImageKey &other) const
    {
        return deviceName == other.deviceName && imageName == other.imageName;
    }
};

struct DeviceImageKeyHash {
    std::size_t operator()(const DeviceImageKey &key) const
    {
        return std::hash<std::string>()(key.deviceName) ^ std::hash<std::string>()(key.imageName);
    }
};

using ProgressBars = std::unordered_map<DeviceImageKey, size_t, DeviceImageKeyHash>;

// Responses are produced by device threads inside the library and consumed
// by main().
inline std::queue<AstraDeviceManagerResponse> g_responses;
inline std::mutex g_responsesMutex;
inline std::condition_variable g_responsesCV;
inline std::atomic<bool> g_running{true};

inline void ResponseCallback(AstraDeviceManagerResponse response)
{
    {
        std::lock_guard<std::mutex> lock(g_responsesMutex);
        g_responses.push(std::move(response));
    }
    g_responsesCV.notify_one();
}

// Signal handlers may only touch lock-free atomics; notifying a condition
// variable from one locks an internal mutex, which is undefined behaviour in
// a handler and can deadlock if the signal lands on a thread already holding
// it.  WaitForResponse() polls g_running instead.
static_assert(std::atomic<bool>::is_always_lock_free,
    "SignalHandler stores to g_running from a signal handler; it must be lock-free");

inline void SignalHandler(int signal)
{
    if (signal == SIGINT) {
        g_running.store(false);
    }
}

inline void InstallSignalHandler()
{
    std::signal(SIGINT, SignalHandler);
}

// Wait briefly for the next response.  Returns nullopt on timeout or when
// shutting down, so callers loop on g_running.
//
// The response is popped and the lock released before it is returned: the
// caller renders progress bars to the terminal, and holding the queue lock
// across that made the device threads block on terminal I/O between chunks.
inline std::optional<AstraDeviceManagerResponse> WaitForResponse()
{
    std::unique_lock<std::mutex> lock(g_responsesMutex);

    g_responsesCV.wait_for(lock, std::chrono::milliseconds(100),
        [] { return !g_responses.empty() || !g_running.load(); });

    if (!g_running.load() || g_responses.empty()) {
        return std::nullopt;
    }

    AstraDeviceManagerResponse response = std::move(g_responses.front());
    g_responses.pop();
    return response;
}

inline void UpdateProgressBars(const DeviceResponse &deviceResponse,
    indicators::DynamicProgress<indicators::ProgressBar> &dynamicProgress,
    ProgressBars &progressBars)
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

    auto &progressBar = dynamicProgress[progressBars[key]];
    progressBar.set_progress(deviceResponse.m_progress);

    if (deviceResponse.m_progress == 100) {
        progressBar.mark_as_completed();
    }
}

inline void UpdateSimpleProgress(const DeviceResponse &deviceResponse)
{
    std::cout << "Device: " << deviceResponse.m_deviceName
              << " Image: " << deviceResponse.m_imageName
              << " Progress: " << deviceResponse.m_progress << std::endl;
}

// Route an image-send response to whichever progress display is in use.
inline void ReportImageProgress(const DeviceResponse &deviceResponse, bool simpleProgress,
    indicators::DynamicProgress<indicators::ProgressBar> &dynamicProgress,
    ProgressBars &progressBars)
{
    if (simpleProgress) {
        UpdateSimpleProgress(deviceResponse);
    } else {
        UpdateProgressBars(deviceResponse, dynamicProgress, progressBars);
    }
}

} // namespace astra_cli
