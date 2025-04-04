// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <atomic>
#include <string>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <fstream>

class AstraConsole
{
public:
    AstraConsole(std::string deviceName, std::string logPath);
    ~AstraConsole();

    void Append(const std::string &data);
    std::string &Get();

    bool WaitForPrompt();
    void Shutdown();

private:
    std::string m_consoleData;
    const std::string m_uBootPrompt = "=>";
    std::condition_variable m_promptCV;
    std::mutex m_promptMutex;
    std::atomic<bool> m_shutdown{false};
    std::ofstream m_consoleLog;
};