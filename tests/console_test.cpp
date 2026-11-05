// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include <chrono>
#include <future>

#include "astra_console.hpp"
#include "test_support.hpp"

namespace {

class PromptWaiter
{
public:
    explicit PromptWaiter(AstraConsole &console)
        : m_console(console), m_result(m_promise.get_future()),
          m_thread([this] {
              try {
                  m_promise.set_value(m_console.WaitForPrompt());
              } catch (...) {
                  m_promise.set_exception(std::current_exception());
              }
          })
    {}

    ~PromptWaiter()
    {
        if (m_thread.joinable()) {
            m_console.Shutdown();
            m_thread.join();
        }
    }

    bool Complete()
    {
        REQUIRE(m_result.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        const bool result = m_result.get();
        m_thread.join();
        return result;
    }

    bool IsPending()
    {
        return m_result.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    }

private:
    AstraConsole &m_console;
    std::promise<bool> m_promise;
    std::future<bool> m_result;
    std::thread m_thread;
};

} // namespace

int main(int argc, char **argv)
{
    test::Suite suite;
    suite.Add("appends-binary-data-and-flushes-log", [] {
        test::TempDirectory directory;
        AstraConsole console("device", directory.Path().string());
        console.Append("first\n");
        console.Append(std::string("a\0b", 3));
        REQUIRE(console.Get() == std::string("first\na\0b", 9));
        REQUIRE(test::ReadFile(directory.Path() / "console.log", std::ios::in) == console.Get());
        console.Shutdown();
    });
    suite.Add("prompt-before-waiter-is-not-lost", [] {
        test::TempDirectory directory;
        AstraConsole console("device", directory.Path().string());
        console.Append("boot output\n=>");
        PromptWaiter waiter(console);
        REQUIRE(waiter.Complete());
        console.Shutdown();
    });
    suite.Add("prompt-after-waiter-wakes-it", [] {
        test::TempDirectory directory;
        AstraConsole console("device", directory.Path().string());
        PromptWaiter waiter(console);
        REQUIRE(waiter.IsPending());
        console.Append("=>");
        REQUIRE(waiter.Complete());
        console.Shutdown();
    });
    for (const std::string suffix : {"", " ", "\t", "\n", "\r\n", "\f\v"}) {
        suite.Add("prompt-trailing-whitespace-" + std::to_string(suffix.size()) +
            (suffix.empty() ? "-empty" : "-" + std::to_string(static_cast<unsigned char>(suffix[0]))), [=] {
            test::TempDirectory directory;
            AstraConsole console("device", directory.Path().string());
            console.Append("=>" + suffix);
            PromptWaiter waiter(console);
            REQUIRE(waiter.Complete());
            console.Shutdown();
        });
    }
    suite.Add("prompt-is-consumed-and-next-wait-needs-new-prompt", [] {
        test::TempDirectory directory;
        AstraConsole console("device", directory.Path().string());
        console.Append("=>");
        PromptWaiter first(console);
        REQUIRE(first.Complete());
        PromptWaiter second(console);
        REQUIRE(second.IsPending());
        console.Append("=>");
        REQUIRE(second.Complete());
        console.Shutdown();
    });
    suite.Add("non-prompt-output-does-not-wake-waiter", [] {
        test::TempDirectory directory;
        AstraConsole console("device", directory.Path().string());
        PromptWaiter waiter(console);
        for (const std::string text : {"", "   \t\n", "=", ">", "=>not a prompt", "ordinary text"}) {
            console.Append(text);
            REQUIRE(waiter.IsPending());
        }
        console.Shutdown();
        REQUIRE(!waiter.Complete());
    });
    suite.Add("shutdown-before-wait-and-repeated-shutdown", [] {
        test::TempDirectory directory;
        AstraConsole console("device", directory.Path().string());
        console.Append("=>");
        console.Shutdown();
        console.Shutdown();
        PromptWaiter waiter(console);
        REQUIRE(!waiter.Complete());
    });
    suite.Add("shutdown-wakes-all-waiters", [] {
        test::TempDirectory directory;
        AstraConsole console("device", directory.Path().string());
        PromptWaiter first(console);
        PromptWaiter second(console);
        REQUIRE(first.IsPending() && second.IsPending());
        console.Shutdown();
        REQUIRE(!first.Complete());
        REQUIRE(!second.Complete());
    });
    suite.Add("characterization-split-prompt-not-detected", [] {
        test::TempDirectory directory;
        AstraConsole console("device", directory.Path().string());
        PromptWaiter waiter(console);
        console.Append("=");
        console.Append(">");
        REQUIRE(waiter.IsPending());
        REQUIRE(console.Get() == "=>");
        console.Shutdown();
        REQUIRE(!waiter.Complete());
    });
    return suite.Run(argc, argv);
}
