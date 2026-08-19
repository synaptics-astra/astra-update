// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#include "astra_log.hpp"
#include <iostream>
#include <mutex>
#include <chrono>
#include <ctime>

std::unique_ptr<AstraLogStore> AstraLogStore::instance;
std::once_flag AstraLogStore::initInstanceFlag;

AstraLog::AstraLog(const std::string &funcName) : m_funcName{funcName}, m_logLevel{ASTRA_LOG_LEVEL_NONE}
{
    // Check the level before formatting: these trace records are emitted on
    // every function entry/exit, including per-transfer USB hot paths, and
    // formatting them only to have Log() filter them out is pure overhead.
    if (AstraLogStore::getInstance().ShouldLog(ASTRA_LOG_LEVEL_TRACE)) {
        AstraLogStore::getInstance().Log(ASTRA_LOG_LEVEL_TRACE,
            AstraLog::FormatLog(ASTRA_LOG_LEVEL_TRACE, m_funcName, "-> Entering"));
    }
}

AstraLog::~AstraLog()
{
    if (AstraLogStore::getInstance().ShouldLog(ASTRA_LOG_LEVEL_TRACE)) {
        AstraLogStore::getInstance().Log(ASTRA_LOG_LEVEL_TRACE,
            AstraLog::FormatLog(ASTRA_LOG_LEVEL_TRACE, m_funcName, "<- Exiting"));
    }
}

AstraLog & AstraLog::operator()(AstraLogLevel level) {
    m_logLevel = level;
    m_enabled = AstraLogStore::getInstance().ShouldLog(level);
    return *this;
}

AstraLog & AstraLog::operator<<(const char *str) {
    if (m_enabled) {
        m_os << str;
    }
    return *this;
}

AstraLog & AstraLog::operator<<(const std::string &str) {
    if (m_enabled) {
        m_os << str;
    }
    return *this;
}

AstraLog & AstraLog::operator<<(int val) {
    if (m_enabled) {
        m_os << val;
    }
    return *this;
}

AstraLog & AstraLog::operator<<(unsigned int val) {
    if (m_enabled) {
        m_os << val;
    }
    return *this;
}

AstraLog & endLog(AstraLog &log) {
    if (log.m_enabled) {
        AstraLogStore::getInstance().Log(log.m_logLevel,
            AstraLog::FormatLog(log.m_logLevel, log.m_funcName, log.m_os.str()));
    }
    log.m_os.str("");
    return log;
}

AstraLog & operator<<(AstraLog &log, AstraLog &(*finalizeLog)(AstraLog &)) {
    return finalizeLog(log);
}

AstraLogLevel AstraLog::StringToLevel(const std::string &level) {
    if (level == "NONE") return ASTRA_LOG_LEVEL_NONE;
    if (level == "TRACE") return ASTRA_LOG_LEVEL_TRACE;
    if (level == "DEBUG") return ASTRA_LOG_LEVEL_DEBUG;
    if (level == "INFO") return ASTRA_LOG_LEVEL_INFO;
    if (level == "WARNING") return ASTRA_LOG_LEVEL_WARNING;
    if (level == "ERROR") return ASTRA_LOG_LEVEL_ERROR;
    throw std::invalid_argument("Unknown log level: " + level);
}

std::string AstraLog::LevelToString(AstraLogLevel level) {
    switch (level) {
        case ASTRA_LOG_LEVEL_NONE: return "NONE";
        case ASTRA_LOG_LEVEL_TRACE: return "TRACE";
        case ASTRA_LOG_LEVEL_DEBUG: return "DEBUG";
        case ASTRA_LOG_LEVEL_INFO: return "INFO";
        case ASTRA_LOG_LEVEL_WARNING: return "WARNING";
        case ASTRA_LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

std::string AstraLog::FormatLog(AstraLogLevel logLevel, const std::string &funcName, const std::string &message) {
    std::ostringstream os;
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    // std::localtime returns a shared static buffer and is not thread-safe;
    // this runs concurrently from every logging thread.
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    // Get microseconds
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;

    os << std::put_time(&tm, "[%H:%M:%S");
    os << '.' << std::setw(6) << std::setfill('0') << us.count() << "]"; // Add microseconds

    std::string levelString = "[" + AstraLog::LevelToString(logLevel) + "]";
    os << "" << std::setfill(' ') << std::setw(9) << std::left << levelString << funcName << ": " << message;
    return os.str();
}

AstraLogStore::AstraLogStore() {}

AstraLogStore::~AstraLogStore() {
    Close();
}

AstraLogStore& AstraLogStore::getInstance() {
    std::call_once(initInstanceFlag, []() {
        instance.reset(new AstraLogStore);
    });
    return *instance;
}

void AstraLogStore::Open(const std::string &logPath, AstraLogLevel minLogLevel) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    m_minLogLevel.store(minLogLevel, std::memory_order_relaxed);

    if (logPath == "" || logPath == "stdout") {
        m_logStream = std::make_unique<std::ostream>(std::cout.rdbuf());
    } else {
        m_logFile.open(logPath, std::ios::out | std::ios::app);
        if (!m_logFile.is_open()) {
            throw std::runtime_error("Failed to open log file");
        }
        m_logStream = std::make_unique<std::ostream>(m_logFile.rdbuf());
    }

    m_opened.store(true, std::memory_order_relaxed);
}

void AstraLogStore::Close() {
    std::lock_guard<std::mutex> lock(m_logMutex);
    // Clear m_opened first so ShouldLog() steers new messages away, then
    // drop the stream under the lock so an in-flight Log() cannot write to
    // a closed file.
    m_opened.store(false, std::memory_order_relaxed);
    m_logStream.reset();
    if (m_logFile.is_open()) {
        m_logFile.close();
    }
}

void AstraLogStore::Log(AstraLogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    if (m_logStream) {
        if (level >= m_minLogLevel.load(std::memory_order_relaxed)) {
            *m_logStream << message << '\n';
            m_logStream->flush();
        }
    }
}