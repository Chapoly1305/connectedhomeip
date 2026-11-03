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
#include <ble/BleTimeout.h>
#include <pw_unit_test/framework.h>

using chip::Ble::ParseBleTimeoutMs;

TEST(TestBleTimeout, PositiveDecimalAndTimerBoundary)
{
    uint32_t value = 0;
    EXPECT_TRUE(ParseBleTimeoutMs("1", value));
    EXPECT_EQ(value, 1u);
    EXPECT_TRUE(ParseBleTimeoutMs("90000", value));
    EXPECT_EQ(value, 90000u);
    EXPECT_TRUE(ParseBleTimeoutMs("00042", value));
    EXPECT_EQ(value, 42u);
    EXPECT_TRUE(ParseBleTimeoutMs("4294967295", value));
    EXPECT_EQ(value, UINT32_MAX);
}

TEST(TestBleTimeout, RejectMalformedOrOverflowAndPreserveDefault)
{
    const char * invalid[] = { nullptr,
                               "",
                               "0",
                               "000",
                               "-1",
                               "+1",
                               " 1",
                               "1 ",
                               "\t1",
                               "1\n",
                               "1ms",
                               "0x10",
                               "4294967296",
                               "4295057296",
                               "18446744073709551615",
                               "18446744073709551616" };
    for (const char * text : invalid)
    {
        uint32_t value = 5000;
        EXPECT_FALSE(ParseBleTimeoutMs(text, value));
        EXPECT_EQ(value, 5000u);
    }
}
