// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include <algorithm>

#include "astra_console.hpp"
#include "astra_log.hpp"

AstraConsole::AstraConsole(std::string deviceName, std::string logPath)
{
    ASTRA_LOG;

    std::string logFile = logPath + "/console.log";
    m_consoleLog = std::ofstream(logFile, std::ios::out | std::ios::trunc);
}

AstraConsole::~AstraConsole()
{
    ASTRA_LOG;
}

void AstraConsole::Append(const std::string &data)
{
    ASTRA_LOG;

    std::string trimmedData = data;
    trimmedData.erase(trimmedData.find_last_not_of(" \t\n\r\f\v") + 1);

    if (trimmedData.size() >= m_uBootPrompt.size() &&
        trimmedData.rfind(m_uBootPrompt) == (trimmedData.size() - m_uBootPrompt.size()))
    {
        log(ASTRA_LOG_LEVEL_DEBUG) << "U-Boot prompt detected." << endLog;
        // Record the prompt under the mutex so a WaitForPrompt() that has not
        // started waiting yet still observes it; a bare notify would be lost.
        {
            std::lock_guard<std::mutex> lock(m_promptMutex);
            m_promptDetected = true;
        }
        m_promptCV.notify_one();
    }

    m_consoleData += data;
    m_consoleLog << data;
    m_consoleLog.flush();
}

std::string &AstraConsole::Get()
{
    ASTRA_LOG;

    return m_consoleData;
}

bool AstraConsole::WaitForPrompt()
{
    ASTRA_LOG;

    std::unique_lock<std::mutex> lock(m_promptMutex);
    m_promptCV.wait(lock, [this] {
        return m_promptDetected || m_shutdown.load();
    });
    if (m_shutdown.load()) {
        return false;
    }

    // Consume the prompt so the next WaitForPrompt() waits for a new one
    // (e.g. the prompt returning after a flash command completes).
    m_promptDetected = false;
    return true;
}

void AstraConsole::Shutdown()
{
    ASTRA_LOG;

    m_shutdown.store(true);
    // Take the lock before notifying so a waiter that just evaluated its
    // predicate (and found it false) is guaranteed to be blocked in wait()
    // before the notification fires.
    {
        std::lock_guard<std::mutex> lock(m_promptMutex);
    }
    m_promptCV.notify_all();
    m_consoleLog.close();
}