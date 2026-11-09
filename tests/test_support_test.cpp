// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#include "test_support.hpp"

int main(int argc, char **argv)
{
    test::Suite suite;
    suite.Add("assertions-remain-active", [] {
        REQUIRE_THROWS(std::runtime_error, REQUIRE(false));
        REQUIRE_THROWS(std::runtime_error, REQUIRE_THROWS(std::invalid_argument, (void)0));
        REQUIRE_THROWS(std::logic_error,
            test::RequireThrows<std::runtime_error>([] { throw std::logic_error("wrong type"); },
                "wrong exception", __FILE__, __LINE__));
    });
    suite.Add("runner-reports-failure-and-continues", [] {
        test::Suite inner;
        bool continued = false;
        inner.Add("failure", [] { REQUIRE(false); });
        inner.Add("following", [&] { continued = true; });
        char program[] = "self-test";
        char *arguments[] = {program};
        test::CaptureStream output(std::cout);
        test::CaptureStream errors(std::cerr);
        REQUIRE(inner.Run(1, arguments) == 1);
        REQUIRE(continued);
        REQUIRE(errors.Get().find("FAIL failure") != std::string::npos);
        REQUIRE(output.Get().find("2 cases, 1 failures") != std::string::npos);
    });
    suite.Add("runner-lists-selects-and-rejects-unknown-case", [] {
        test::Suite inner;
        int executed = 0;
        inner.Add("selected", [&] { ++executed; });
        inner.Add("other", [] { REQUIRE(false); });
        REQUIRE_THROWS(std::invalid_argument, inner.Add("selected", [] {}));
        char program[] = "self-test";
        char list[] = "--list";
        char selected[] = "selected";
        char unknown[] = "unknown";
        char *arguments[] = {program, list};
        test::CaptureStream output(std::cout);
        test::CaptureStream errors(std::cerr);
        REQUIRE(inner.Run(2, arguments) == 0);
        REQUIRE(executed == 0);
        REQUIRE(output.Get() == "selected\nother\n");
        arguments[1] = selected;
        REQUIRE(inner.Run(2, arguments) == 0);
        REQUIRE(executed == 1);
        arguments[1] = unknown;
        REQUIRE(inner.Run(2, arguments) == 1);
        REQUIRE(errors.Get().find("No test case selected") != std::string::npos);
        REQUIRE(inner.Run(3, arguments) == 1);
    });
    suite.Add("fixtures-own-and-clean-only-their-directory", [] {
        test::TempDirectory unrelated;
        const auto sentinel = unrelated.Write("keep.txt", "keep");
        std::filesystem::path owned;
        {
            test::TempDirectory directory;
            owned = directory.Path();
            REQUIRE(owned != unrelated.Path());
            const auto file = directory.Write(std::filesystem::path("nested") / "data.bin",
                std::string("a\0b", 3));
            REQUIRE(test::ReadFile(file) == std::string("a\0b", 3));
        }
        REQUIRE(!std::filesystem::exists(owned));
        REQUIRE(test::ReadFile(sentinel) == "keep");
    });
    suite.Add("runner-makes-skipped-cases-visible", [] {
        test::Suite inner;
        inner.Add("unavailable", [] { throw test::Skip("prerequisite unavailable"); });
        char program[] = "self-test";
        char *arguments[] = {program};
        test::CaptureStream output(std::cout);
        REQUIRE(inner.Run(1, arguments) == 0);
        REQUIRE(output.Get().find("SKIP unavailable: prerequisite unavailable") != std::string::npos);
        REQUIRE(output.Get().find("1 skipped cases") != std::string::npos);
    });
    return suite.Run(argc, argv);
}
