// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <atomic>
#include <sstream>
#include <string>
#include <fstream>
#include <memory>
#include <mutex>
#include <iomanip>

enum AstraLogLevel {
    ASTRA_LOG_LEVEL_TRACE,
    ASTRA_LOG_LEVEL_DEBUG,
    ASTRA_LOG_LEVEL_INFO,
    ASTRA_LOG_LEVEL_WARNING,
    ASTRA_LOG_LEVEL_ERROR,
    ASTRA_LOG_LEVEL_NONE
};

class AstraLog {
public:
    std::ostringstream m_os;
    std::string m_funcName;
    AstraLogLevel m_logLevel;
    // Set by operator() based on the store's minimum level; when false the
    // stream operators and endLog skip all formatting work.
    bool m_enabled = false;

    AstraLog(const std::string &funcName);
    ~AstraLog();

    AstraLog & operator()(AstraLogLevel level);
    AstraLog & operator<<(const char *str);
    AstraLog & operator<<(const std::string &str);
    AstraLog & operator<<(int val);
    AstraLog & operator<<(unsigned int val);

    template <typename T>
    AstraLog & operator<<(T manupulator) {
        if (m_enabled) {
            m_os << manupulator;
        }
        return *this;
    }

    static AstraLogLevel StringToLevel(const std::string &level);
    static std::string LevelToString(AstraLogLevel level);
    static std::string FormatLog(AstraLogLevel level, const
        std::string &funcName, const std::string &message);

};

AstraLog & endLog(AstraLog &log);
AstraLog & operator<<(AstraLog &log, AstraLog &(*finalizeLog)(AstraLog &));

class AstraLogStore {
public:
    static AstraLogStore& getInstance();
    void Log(AstraLogLevel level, const std::string& message);
    AstraLogLevel GetMinLogLevel() const { return m_minLogLevel.load(std::memory_order_relaxed); }

    // Cheap lock-free check used by call sites to skip message formatting
    // entirely when the store is closed or the level is filtered out.
    bool ShouldLog(AstraLogLevel level) const {
        return m_opened.load(std::memory_order_relaxed) &&
            level >= m_minLogLevel.load(std::memory_order_relaxed);
    }

    void Open(const std::string &logPath, AstraLogLevel minLogLevel);
    void Close();
    ~AstraLogStore();

private:
    AstraLogStore();
    AstraLogStore(const AstraLogStore&) = delete;
    AstraLogStore& operator=(const AstraLogStore&) = delete;

    std::unique_ptr<std::ostream> m_logStream;
    std::ofstream m_logFile;
    std::atomic<AstraLogLevel> m_minLogLevel{ASTRA_LOG_LEVEL_NONE};
    std::atomic<bool> m_opened{false};
    // Serializes writes to m_logStream; messages are logged from many
    // threads (device threads, USB event threads, reader threads).
    std::mutex m_logMutex;
    static std::unique_ptr<AstraLogStore> instance;
    static std::once_flag initInstanceFlag;
};

#define ASTRA_LOG AstraLog log(__FUNCTION__)