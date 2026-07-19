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

#include "FakeBleTransport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/PlatformManager.h>

namespace chip {
namespace DeviceLayer {
namespace Internal {

namespace {

// Frame type bytes -- must match src/platform/silabs/efr32/FakeBLETransport.cpp on the device.
enum class FrameType : uint8_t
{
    kConnect      = 0x01,
    kDisconnect   = 0x02,
    kWriteRequest = 0x03,
    kSubscribe    = 0x04,
    kUnsubscribe  = 0x05,
    kIndication   = 0x06,
};

constexpr uint16_t kMaxFramePayload = 512;
// The fake transport only ever talks to a single peer; use a fixed non-null connection object.
// BLE_CONNECTION_OBJECT is a typed pointer (BluezConnection*) on the Linux platform, so use that
// type directly rather than void* (which does not implicitly convert at the BleLayer call sites).
BLE_CONNECTION_OBJECT const kFakeConnObj = reinterpret_cast<BLE_CONNECTION_OBJECT>(0x1);

int gSocketFd            = -1;
Ble::BleLayer * gBleLayer = nullptr;
std::thread * gRxThread   = nullptr;

bool SendFrame(FrameType type, const uint8_t * payload, uint16_t len)
{
    if (gSocketFd < 0)
    {
        return false;
    }
    uint8_t header[3] = { static_cast<uint8_t>(type), static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>((len >> 8) & 0xFF) };
    if (write(gSocketFd, header, sizeof(header)) != static_cast<ssize_t>(sizeof(header)))
    {
        return false;
    }
    if (len > 0 && write(gSocketFd, payload, len) != static_cast<ssize_t>(len))
    {
        return false;
    }
    return true;
}

bool BlockingReadExact(uint8_t * buf, size_t length)
{
    size_t received = 0;
    while (received < length)
    {
        ssize_t n = read(gSocketFd, buf + received, length - received);
        if (n <= 0)
        {
            return false;
        }
        received += static_cast<size_t>(n);
    }
    return true;
}

void RxThreadMain()
{
    uint8_t header[3];
    uint8_t payload[kMaxFramePayload];

    while (BlockingReadExact(header, sizeof(header)))
    {
        FrameType type = static_cast<FrameType>(header[0]);
        uint16_t len   = static_cast<uint16_t>(header[1]) | (static_cast<uint16_t>(header[2]) << 8);
        VerifyOrDie(len <= kMaxFramePayload);
        if (len > 0 && !BlockingReadExact(payload, len))
        {
            break;
        }

        if (type == FrameType::kIndication && gBleLayer != nullptr)
        {
            System::PacketBufferHandle buf = System::PacketBufferHandle::NewWithData(payload, len, 0, 0);
            if (!buf.IsNull())
            {
                // Real BLE platforms deliver GATT events on the CHIP event-loop thread. Do the same:
                // marshal the indication onto that thread via ScheduleWork instead of calling the
                // BleLayer from this raw RX thread. This is both thread-safe (no chipDie unsafe-access
                // abort) and ensures the event loop then drives BTP sending of the follow-up data.
                LogErrorOnFailure(DeviceLayer::PlatformMgr().ScheduleWork(
                    [](intptr_t ctx) {
                        System::PacketBufferHandle b =
                            System::PacketBufferHandle::Adopt(reinterpret_cast<System::PacketBuffer *>(ctx));
                        if (gBleLayer != nullptr)
                        {
                            gBleLayer->HandleIndicationReceived(kFakeConnObj, &Ble::CHIP_BLE_SVC_ID,
                                                                &Ble::CHIP_BLE_CHAR_2_UUID, std::move(b));
                        }
                    },
                    reinterpret_cast<intptr_t>(std::move(buf).UnsafeRelease())));
            }
        }
    }

    ChipLogProgress(Ble, "FakeBleTransport: peer connection closed");
    if (gBleLayer != nullptr)
    {
        LogErrorOnFailure(DeviceLayer::PlatformMgr().ScheduleWork(
            [](intptr_t) {
                if (gBleLayer != nullptr)
                {
                    gBleLayer->HandleConnectionError(kFakeConnObj, CHIP_ERROR_CONNECTION_ABORTED);
                }
            },
            0));
    }
}

} // namespace

Optional<uint16_t> GetFakeBleTransportPort()
{
    const char * portStr = getenv("CHIP_FAKE_BLE_PORT");
    if (portStr == nullptr || portStr[0] == '\0')
    {
        return Optional<uint16_t>::Missing();
    }
    return Optional<uint16_t>::Value(static_cast<uint16_t>(atoi(portStr)));
}

void FakeBleConnectionDelegate::NewConnection(Ble::BleLayer * bleLayer, void * appState, const SetupDiscriminator & /* connDiscriminator */)
{
    // Fake transport assumption: there is exactly one known peer (Renode's exposed socket), so
    // discovery/scanning is skipped entirely -- just connect directly.
    ChipLogProgress(Ble, "FakeBleTransport: connecting to fake BLE peer on port %u", mPort);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        ChipLogError(Ble, "FakeBleTransport: socket() failed: %s", strerror(errno));
        BleConnectionDelegate::OnConnectionError(appState, CHIP_ERROR_INTERNAL);
        return;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(mPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        ChipLogError(Ble, "FakeBleTransport: connect() failed: %s", strerror(errno));
        close(fd);
        BleConnectionDelegate::OnConnectionError(appState, CHIP_ERROR_INTERNAL);
        return;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    gSocketFd  = fd;
    gBleLayer  = bleLayer;
    gRxThread  = new std::thread(RxThreadMain);

    if (!SendFrame(FrameType::kConnect, nullptr, 0))
    {
        ChipLogError(Ble, "FakeBleTransport: failed to send CONNECT frame");
        BleConnectionDelegate::OnConnectionError(appState, CHIP_ERROR_INTERNAL);
        return;
    }

    ChipLogProgress(Ble, "FakeBleTransport: connected");
    BleConnectionDelegate::OnConnectionComplete(appState, kFakeConnObj);
}

void FakeBleConnectionDelegate::NewConnection(Ble::BleLayer * bleLayer, void * appState, BLE_CONNECTION_OBJECT /* connObj */)
{
    // Reconnecting to an already-known object is equivalent to a fresh connect in this harness.
    SetupDiscriminator unused;
    NewConnection(bleLayer, appState, unused);
}

CHIP_ERROR FakeBleConnectionDelegate::CancelConnection()
{
    return CHIP_NO_ERROR;
}

CHIP_ERROR FakeBlePlatformDelegate::SubscribeCharacteristic(BLE_CONNECTION_OBJECT /* connObj */, const Ble::ChipBleUUID * /* svcId */,
                                                            const Ble::ChipBleUUID * /* charId */)
{
    if (!SendFrame(FrameType::kSubscribe, nullptr, 0))
    {
        return CHIP_ERROR_INTERNAL;
    }
    // A real GATT subscribe (C2 CCCD write) completes with a subscribe-complete callback, which the BTP
    // engine needs to clear kGattOperationInFlight and continue the handshake (it prompts the peripheral
    // to send its capabilities indication). Synthesize that completion on the CHIP event-loop thread --
    // without it the central stalls with "Gatt op in flight" and never sends the PASE request.
    LogErrorOnFailure(DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t) {
            if (gBleLayer != nullptr)
            {
                gBleLayer->HandleSubscribeComplete(kFakeConnObj, &Ble::CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_2_UUID);
            }
        },
        0));
    return CHIP_NO_ERROR;
}

CHIP_ERROR FakeBlePlatformDelegate::UnsubscribeCharacteristic(BLE_CONNECTION_OBJECT /* connObj */,
                                                              const Ble::ChipBleUUID * /* svcId */,
                                                              const Ble::ChipBleUUID * /* charId */)
{
    return SendFrame(FrameType::kUnsubscribe, nullptr, 0) ? CHIP_NO_ERROR : CHIP_ERROR_INTERNAL;
}

CHIP_ERROR FakeBlePlatformDelegate::CloseConnection(BLE_CONNECTION_OBJECT /* connObj */)
{
    SendFrame(FrameType::kDisconnect, nullptr, 0);
    if (gSocketFd >= 0)
    {
        close(gSocketFd);
        gSocketFd = -1;
    }
    return CHIP_NO_ERROR;
}

uint16_t FakeBlePlatformDelegate::GetMTU(BLE_CONNECTION_OBJECT /* connObj */) const
{
    return 247;
}

CHIP_ERROR FakeBlePlatformDelegate::SendIndication(BLE_CONNECTION_OBJECT /* connObj */, const Ble::ChipBleUUID * /* svcId */,
                                                   const Ble::ChipBleUUID * /* charId */, System::PacketBufferHandle /* pBuf */)
{
    // The controller never indicates to the device in CHIPoBLE -- only the peripheral does.
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR FakeBlePlatformDelegate::SendWriteRequest(BLE_CONNECTION_OBJECT /* connObj */, const Ble::ChipBleUUID * /* svcId */,
                                                     const Ble::ChipBleUUID * /* charId */, System::PacketBufferHandle pBuf)
{
    bool ok = SendFrame(FrameType::kWriteRequest, pBuf->Start(), static_cast<uint16_t>(pBuf->DataLength()));
    // A real GATT write completes with a write-confirmation callback, which the BTP engine needs to
    // clear kGattOperationInFlight and send the next fragment/message. The fake pipe is reliable, so
    // synthesize that confirmation on the CHIP event-loop thread. Without it the BTP engine stalls
    // after the first write and never sends the PASE PBKDFParamRequest.
    if (ok)
    {
        LogErrorOnFailure(DeviceLayer::PlatformMgr().ScheduleWork(
            [](intptr_t) {
                if (gBleLayer != nullptr)
                {
                    gBleLayer->HandleWriteConfirmation(kFakeConnObj, &Ble::CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_1_UUID);
                }
            },
            0));
    }
    return ok ? CHIP_NO_ERROR : CHIP_ERROR_INTERNAL;
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
