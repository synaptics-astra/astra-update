// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <chrono>
#include <iomanip>
#include <future>
#include <regex>
#include <set>
#include <thread>

#include "log_fixture.hpp"
#include "test_support.hpp"

namespace {

struct LevelCase {
    AstraLogLevel level;
    const char *name;
};

constexpr LevelCase levels[] = {
    {ASTRA_LOG_LEVEL_TRACE, "TRACE"}, {ASTRA_LOG_LEVEL_DEBUG, "DEBUG"}, {ASTRA_LOG_LEVEL_INFO, "INFO"},
    {ASTRA_LOG_LEVEL_WARNING, "WARNING"}, {ASTRA_LOG_LEVEL_ERROR, "ERROR"}, {ASTRA_LOG_LEVEL_NONE, "NONE"}
};

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    for (const auto level : levels) {
        suite.Add(std::string("level-") + level.name, [=] {
            REQUIRE(AstraLog::StringToLevel(level.name) == level.level);
            REQUIRE(AstraLog::LevelToString(level.level) == level.name);
        });
        suite.Add(std::string("minimum-level-") + level.name, [=] {
            test::TempDirectory directory;
            const auto path = directory.Path() / "levels.log";
            {
                test::LogSession session(path.string(), level.level);
                auto &store = AstraLogStore::getInstance();
                REQUIRE(store.GetMinLogLevel() == level.level);
                for (const auto candidate : levels) {
                    REQUIRE(store.ShouldLog(candidate.level) == (candidate.level >= level.level));
                    store.Log(candidate.level, candidate.name);
                }
            }
            const auto content = test::ReadFile(path, std::ios::in);
            for (const auto candidate : levels) {
                REQUIRE((content.find(std::string(candidate.name) + '\n') != std::string::npos) ==
                    (candidate.level >= level.level));
            }
        });
    }
    suite.Add("invalid-log-level", [] {
        REQUIRE_THROWS(std::invalid_argument, AstraLog::StringToLevel("info"));
        REQUIRE_THROWS(std::invalid_argument, AstraLog::StringToLevel(""));
        REQUIRE_THROWS(std::invalid_argument, AstraLog::StringToLevel("unknown"));
        REQUIRE(AstraLog::LevelToString(static_cast<AstraLogLevel>(6)) == "UNKNOWN");
    });
    suite.Add("formatted-record-shape", [] {
        const auto message = AstraLog::FormatLog(ASTRA_LOG_LEVEL_INFO, "function", "message");
        REQUIRE(std::regex_match(message,
            std::regex(R"(\[[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6}\]\[INFO\]   function: message)")));
    });
    suite.Add("closed-and-filtered-streams-do-not-format", [] {
        auto &store = AstraLogStore::getInstance();
        store.Close();
        REQUIRE(!store.ShouldLog(ASTRA_LOG_LEVEL_ERROR));
        AstraLog closed("closed");
        closed(ASTRA_LOG_LEVEL_ERROR) << "ignored" << endLog;
        REQUIRE(!closed.m_enabled && closed.m_os.str().empty());
        test::TempDirectory directory;
        test::LogSession session((directory.Path() / "filtered.log").string(), ASTRA_LOG_LEVEL_ERROR);
        AstraLog filtered("filtered");
        filtered(ASTRA_LOG_LEVEL_DEBUG) << "ignored" << 42 << std::hex << 255U << endLog;
        REQUIRE(!filtered.m_enabled && filtered.m_os.str().empty());
    });
    suite.Add("enabled-overloads-manipulators-and-buffer-reset", [] {
        test::TempDirectory directory;
        const auto path = directory.Path() / "streams.log";
        {
            test::LogSession session(path.string(), ASTRA_LOG_LEVEL_INFO);
            AstraLog log("streams");
            log(ASTRA_LOG_LEVEL_INFO) << "string " << std::string("value ") << -2 << ' '
                << 3U << ' ' << std::hex << 255U << endLog;
            REQUIRE(log.m_os.str().empty());
            log(ASTRA_LOG_LEVEL_ERROR) << "next" << endLog;
        }
        const auto data = test::ReadFile(path, std::ios::in);
        REQUIRE(data.find("streams: string value -2 3 ff\n") != std::string::npos);
        REQUIRE(data.find("streams: next\n") != std::string::npos);
        REQUIRE(data.find("nextstring") == std::string::npos);
    });
    suite.Add("trace-entry-exit-gating", [] {
        test::TempDirectory directory;
        const auto path = directory.Path() / "trace.log";
        {
            test::LogSession session(path.string(), ASTRA_LOG_LEVEL_TRACE);
            AstraLog log("traced");
        }
        auto content = test::ReadFile(path, std::ios::in);
        REQUIRE(content.find("traced: -> Entering") != std::string::npos);
        REQUIRE(content.find("traced: <- Exiting") != std::string::npos);
        {
            test::LogSession session(path.string(), ASTRA_LOG_LEVEL_DEBUG);
            AstraLog log("not-traced");
        }
        content = test::ReadFile(path, std::ios::in);
        REQUIRE(content.find("not-traced") == std::string::npos);
    });
    suite.Add("append-close-reopen-and-flush", [] {
        test::TempDirectory directory;
        const auto path = directory.Write("append.log", "existing\n");
        auto &store = AstraLogStore::getInstance();
        {
            test::LogSession session(path.string(), ASTRA_LOG_LEVEL_INFO);
            store.Log(ASTRA_LOG_LEVEL_INFO, "first");
            REQUIRE(test::ReadFile(path, std::ios::in) == "existing\nfirst\n");
        }
        store.Close();
        store.Log(ASTRA_LOG_LEVEL_ERROR, "closed");
        REQUIRE(!store.ShouldLog(ASTRA_LOG_LEVEL_ERROR));
        {
            test::LogSession session(path.string(), ASTRA_LOG_LEVEL_ERROR);
            store.Log(ASTRA_LOG_LEVEL_ERROR, "second");
        }
        REQUIRE(test::ReadFile(path, std::ios::in) == "existing\nfirst\nsecond\n");
    });
    for (const std::string path : {"stdout", ""}) {
        suite.Add(path.empty() ? "empty-path-uses-stdout" : "stdout-path", [=] {
            test::CaptureStream output(std::cout);
            {
                test::LogSession session(path, ASTRA_LOG_LEVEL_INFO);
                AstraLogStore::getInstance().Log(ASTRA_LOG_LEVEL_INFO, "stdout message");
            }
            REQUIRE(output.Get() == "stdout message\n");
        });
    }
    suite.Add("open-failure-and-recovery", [] {
        test::TempDirectory directory;
        auto &store = AstraLogStore::getInstance();
        store.Close();
        REQUIRE_THROWS(std::runtime_error,
            store.Open((directory.Path() / "missing" / "log.txt").string(), ASTRA_LOG_LEVEL_INFO));
        REQUIRE(!store.ShouldLog(ASTRA_LOG_LEVEL_INFO));
        test::LogSession session((directory.Path() / "recovered.log").string(), ASTRA_LOG_LEVEL_INFO);
        REQUIRE(store.ShouldLog(ASTRA_LOG_LEVEL_INFO));
        store.Log(ASTRA_LOG_LEVEL_INFO, "recovered");
    });
    suite.Add("concurrent-writes-preserve-whole-records", [] {
        test::TempDirectory directory;
        const auto path = directory.Path() / "threads.log";
        {
            test::LogSession session(path.string(), ASTRA_LOG_LEVEL_INFO);
            test::ThreadGroup threads;
            for (int worker = 0; worker < 4; ++worker) {
                threads.Add([=] {
                    for (int record = 0; record < 50; ++record) {
                        AstraLog log("concurrent");
                        log(ASTRA_LOG_LEVEL_INFO) << "worker-" << worker << "-record-" << record << endLog;
                    }
                });
            }
            threads.Join();
        }
        std::istringstream lines(test::ReadFile(path, std::ios::in));
        std::set<std::string> records;
        std::string line;
        while (std::getline(lines, line)) {
            REQUIRE(std::regex_match(line, std::regex(
                R"(\[[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6}\]\[INFO\]   concurrent: worker-[0-3]-record-[0-9]+)")));
            records.insert(line.substr(line.find("worker-")));
        }
        REQUIRE(records.size() == 200);
    });
    suite.Add("close-while-writer-is-active-is-safe", [] {
        test::TempDirectory directory;
        const auto path = directory.Path() / "close.log";
        std::promise<void> firstRecord;
        auto started = firstRecord.get_future();
        test::LogSession session(path.string(), ASTRA_LOG_LEVEL_INFO);
        test::ThreadGroup threads;
        threads.Add([&] {
            auto &store = AstraLogStore::getInstance();
            store.Log(ASTRA_LOG_LEVEL_INFO, "first");
            firstRecord.set_value();
            for (int i = 0; i < 1000; ++i) {
                store.Log(ASTRA_LOG_LEVEL_INFO, "record");
            }
        });
        REQUIRE(started.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        AstraLogStore::getInstance().Close();
        threads.Join();
        REQUIRE(!AstraLogStore::getInstance().ShouldLog(ASTRA_LOG_LEVEL_INFO));
        std::istringstream lines(test::ReadFile(path, std::ios::in));
        std::string line;
        REQUIRE(static_cast<bool>(std::getline(lines, line)) && line == "first");
        while (std::getline(lines, line)) {
            REQUIRE(line == "record");
        }
    });
    return suite.Run(argc, argv);
}
