// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Synaptics Incorporated

#pragma once

#include <cstdint>
#include <cstddef>

class Device {
public:
    Device()
    {}
    virtual ~Device()
    {}

    virtual int Open()
    {
        return 0;
    }
    virtual void Close() = 0;

    virtual int Write(uint8_t *data, size_t size, int *transferred) = 0;
};