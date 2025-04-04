// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

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

    AstraLog(const std::string &funcName);
    ~AstraLog();

    AstraLog & operator()(AstraLogLevel level);
    AstraLog & operator<<(const char *str);
    AstraLog & operator<<(const std::string &str);
    AstraLog & operator<<(int val);
    AstraLog & operator<<(unsigned int val);

    template <typename T>
    AstraLog & operator<<(T manupulator) {
        m_os << manupulator;
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
    AstraLogLevel GetMinLogLevel() const { return m_minLogLevel; }
    void Open(const std::string &logPath, AstraLogLevel minLogLevel);
    void Close();
    ~AstraLogStore();

private:
    AstraLogStore();
    AstraLogStore(const AstraLogStore&) = delete;
    AstraLogStore& operator=(const AstraLogStore&) = delete;

    std::unique_ptr<std::ostream> m_logStream;
    std::ofstream m_logFile;
    AstraLogLevel m_minLogLevel;
    static std::unique_ptr<AstraLogStore> instance;
    static std::once_flag initInstanceFlag;
};

#define ASTRA_LOG AstraLog log(__FUNCTION__)