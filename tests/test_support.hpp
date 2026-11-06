// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace test {

inline std::atomic<unsigned int> cleanupFailures{0};

class Skip : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

inline void Require(bool condition, const char *expression, const char *file, int line)
{
    if (!condition) {
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
            ": " + expression);
    }
}

template <typename Exception, typename Function>
void RequireThrows(Function function, const char *expression, const char *file, int line)
{
    try {
        function();
    } catch (const Exception &) {
        return;
    }
    Require(false, expression, file, line);
}

class Suite
{
public:
    void Add(std::string name, std::function<void()> run)
    {
        for (const auto &entry : m_cases) {
            if (entry.first == name) {
                throw std::invalid_argument("Duplicate test case: " + name);
            }
        }
        m_cases.emplace_back(std::move(name), std::move(run));
    }

    int Run(int argc, char **argv) const
    {
        if (argc > 2) {
            std::cerr << "Usage: " << argv[0] << " [--list | exact-case-name]\n";
            return 1;
        }
        const std::string filter = argc == 2 ? argv[1] : "";
        if (filter == "--list") {
            for (const auto &entry : m_cases) {
                std::cout << entry.first << '\n';
            }
            return 0;
        }
        unsigned int failures = 0;
        unsigned int skipped = 0;
        unsigned int executed = 0;
        const auto previousCleanupFailures = cleanupFailures.load();
        for (const auto &entry : m_cases) {
            if (!filter.empty() && entry.first != filter) {
                continue;
            }
            ++executed;
            try {
                entry.second();
                std::cout << "PASS " << entry.first << '\n';
            } catch (const Skip &reason) {
                std::cout << "SKIP " << entry.first << ": " << reason.what() << '\n';
                ++skipped;
            } catch (const std::exception &error) {
                std::cerr << "FAIL " << entry.first << ": " << error.what() << '\n';
                ++failures;
            } catch (...) {
                std::cerr << "FAIL " << entry.first << ": non-standard exception\n";
                ++failures;
            }
        }
        if (executed == 0) {
            std::cerr << "No test case selected: " << filter << '\n';
            return 1;
        }
        std::cout << executed << " cases, " << failures << " failures\n";
        if (skipped != 0) {
            std::cout << skipped << " skipped cases\n";
        }
        return failures == 0 && cleanupFailures.load() == previousCleanupFailures ? 0 : 1;
    }

private:
    std::vector<std::pair<std::string, std::function<void()>>> m_cases;
};

class TempDirectory
{
public:
    TempDirectory()
    {
        std::random_device random;
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() /
                ("astra-unit-test-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate)) {
                m_path = candidate;
                return;
            }
        }
        throw std::runtime_error("Unable to create a unique test directory");
    }

    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
        if (error) {
            ++cleanupFailures;
            std::cerr << "Fixture cleanup failed for " << m_path << ": " << error.message() << '\n';
        }
    }

    TempDirectory(const TempDirectory &) = delete;
    TempDirectory &operator=(const TempDirectory &) = delete;

    const std::filesystem::path &Path() const { return m_path; }

    std::filesystem::path Write(const std::filesystem::path &relativePath, const std::string &data) const
    {
        const auto path = m_path / relativePath;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file;
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file.open(path, std::ios::binary);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.close();
        return path;
    }

private:
    std::filesystem::path m_path;
};

inline std::string ReadFile(const std::filesystem::path &path,
    std::ios::openmode mode = std::ios::binary)
{
    std::ifstream file(path, mode);
    if (!file) {
        throw std::runtime_error("Cannot read test file: " + path.string());
    }
    std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (file.bad()) {
        throw std::runtime_error("Failed to read test file: " + path.string());
    }
    return data;
}

class CaptureStream
{
public:
    explicit CaptureStream(std::ostream &stream) : m_stream(stream), m_previous(stream.rdbuf(m_data.rdbuf())) {}
    ~CaptureStream() { m_stream.rdbuf(m_previous); }
    CaptureStream(const CaptureStream &) = delete;
    CaptureStream &operator=(const CaptureStream &) = delete;
    std::string Get() const { return m_data.str(); }

private:
    std::ostringstream m_data;
    std::ostream &m_stream;
    std::streambuf *m_previous;
};

class ThreadGroup
{
public:
    ~ThreadGroup() { Join(); }
    void Add(std::function<void()> work) { m_threads.emplace_back(std::move(work)); }
    void Join()
    {
        for (auto &thread : m_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

private:
    std::vector<std::thread> m_threads;
};

} // namespace test

#define REQUIRE(...) test::Require((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define REQUIRE_THROWS(exception, ...) \
    test::RequireThrows<exception>([&] { __VA_ARGS__; }, #__VA_ARGS__ " throws " #exception, __FILE__, __LINE__)
