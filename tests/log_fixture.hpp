// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Synaptics Incorporated

#pragma once

#include "astra_log.hpp"

namespace test {

class LogSession
{
public:
    LogSession(const std::string &path, AstraLogLevel level)
    {
        AstraLogStore::getInstance().Open(path, level);
    }

    ~LogSession() { AstraLogStore::getInstance().Close(); }
    LogSession(const LogSession &) = delete;
    LogSession &operator=(const LogSession &) = delete;
};

} // namespace test
