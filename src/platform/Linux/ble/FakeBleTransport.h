/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 * @file
 *   Software-only BLE central used to commission a Matter device running inside the Renode
 *   emulator, bypassing BlueZ/real BLE entirely. Talks to the device's FakeBLETransport
 *   (src/platform/silabs/efr32/FakeBLETransport.cpp) over a TCP socket exposed by Renode's
 *   `CreateServerSocketTerminal`, using the same framed byte protocol.
 *
 *   Activated only when CHIP_FAKE_BLE_PORT is set in the environment (see
 *   MaybeGetFakeBleHostPort() / --fake-ble-port in chip-tool's pairing command).
 */

#pragma once

#include <ble/Ble.h>
#include <lib/core/Optional.h>

namespace chip {
namespace DeviceLayer {
namespace Internal {

// Returns the port to connect to if the fake BLE transport should be used for this run (set via
// CHIP_FAKE_BLE_PORT env var or chip-tool's --fake-ble-port), or nullopt for the real BLE path.
Optional<uint16_t> GetFakeBleTransportPort();

class FakeBleConnectionDelegate : public Ble::BleConnectionDelegate
{
public:
    explicit FakeBleConnectionDelegate(uint16_t port) : mPort(port) {}

    void NewConnection(Ble::BleLayer * bleLayer, void * appState, const SetupDiscriminator & connDiscriminator) override;
    void NewConnection(Ble::BleLayer * bleLayer, void * appState, BLE_CONNECTION_OBJECT connObj) override;
    CHIP_ERROR CancelConnection() override;

private:
    uint16_t mPort;
};

class FakeBlePlatformDelegate : public Ble::BlePlatformDelegate
{
public:
    CHIP_ERROR SubscribeCharacteristic(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                       const Ble::ChipBleUUID * charId) override;
    CHIP_ERROR UnsubscribeCharacteristic(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId,
                                         const Ble::ChipBleUUID * charId) override;
    CHIP_ERROR CloseConnection(BLE_CONNECTION_OBJECT connObj) override;
    uint16_t GetMTU(BLE_CONNECTION_OBJECT connObj) const override;
    CHIP_ERROR SendIndication(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId,
                              System::PacketBufferHandle pBuf) override;
    CHIP_ERROR SendWriteRequest(BLE_CONNECTION_OBJECT connObj, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId,
                                System::PacketBufferHandle pBuf) override;
};

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
