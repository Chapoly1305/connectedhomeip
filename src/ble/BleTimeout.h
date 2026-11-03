/*
 * Copyright (c) 2026 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <cstdint>
#include <limits>

namespace chip {
namespace Ble {

// Accept only a positive decimal value representable by the timer. Leave the
// caller's default unchanged on malformed input or overflow, on 32/64-bit hosts.
inline bool ParseBleTimeoutMs(const char * text, uint32_t & milliseconds)
{
    if (text == nullptr || *text == '\0')
    {
        return false;
    }
    uint32_t value = 0;
    for (; *text != '\0'; ++text)
    {
        if (*text < '0' || *text > '9')
        {
            return false;
        }
        const auto digit = static_cast<uint32_t>(*text - '0');
        if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10)
        {
            return false;
        }
        value = value * 10 + digit;
    }
    if (value == 0)
    {
        return false;
    }
    milliseconds = value;
    return true;
}

} // namespace Ble
} // namespace chip
