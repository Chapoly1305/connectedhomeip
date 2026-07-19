/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
 *    All rights reserved.
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
 *    @file
 *      RENODE EMULATION ONLY. A raw CHIP transport that carries OPERATIONAL
 *      (post-commissioning) Matter messages over a simple byte-pipe instead of
 *      UDP-over-Thread. It exists because the emulated single-node Thread device
 *      has no border router / SRP server / DNS-SD path the host can reach, so
 *      chip-tool's normal operational discovery + CASE-over-UDP cannot complete.
 *
 *      This mirrors the "fake CHIPoBLE transport" used for commissioning, but for
 *      the operational path. Both directions frame a CHIP message packet as
 *      [0x10][2B len LE][payload] on the SAME EUSART1/socket pipe that carries the
 *      fake-BLE frames (demuxed by the leading type byte). The actual byte I/O is
 *      supplied per-platform via FakeOperationalPlatformSend() (device = SiLabs
 *      FakeBLETransport.cpp UARTDRV; host = Linux FakeBleTransport.cpp socket).
 *
 *      Routing is keyed on PeerAddress Type::kFakeOperational. Because the pipe is
 *      lossless (like TCP), MRP is left disabled for this type (see
 *      SecureSession::AllowsMRP); each CASE/Invoke response implicitly acks.
 */

#pragma once

#include <lib/core/CHIPCore.h>
#include <lib/support/DLLUtil.h>
#include <system/SystemPacketBuffer.h>
#include <transport/raw/Base.h>
#include <transport/raw/PeerAddress.h>

#include <cstddef>
#include <cstdint>

namespace chip {
namespace Transport {

/**
 * Platform-provided byte-pipe writer, registered at boot via
 * FakeOperational::SetPlatformSend(). Frames `data`/`len` as an operational
 * packet and pushes it out the shared pipe (device = SiLabs FakeBLETransport.cpp
 * UARTDRV; host = Linux FakeBleTransport.cpp socket).
 */
typedef void (*FakeOperationalSendFn)(const uint8_t * data, size_t len);

/** Listen parameters (none needed; the pipe is platform-owned). */
class FakeOperationalListenParameters
{
public:
    FakeOperationalListenParameters()                                        = default;
    FakeOperationalListenParameters(const FakeOperationalListenParameters &) = default;
    FakeOperationalListenParameters(FakeOperationalListenParameters &&)      = default;
};

class DLL_EXPORT FakeOperational : public Base
{
public:
    FakeOperational() {}
    ~FakeOperational() override { Close(); }

    CHIP_ERROR Init(const FakeOperationalListenParameters &)
    {
        sInstance = this;
        return CHIP_NO_ERROR;
    }

    void Close() override
    {
        if (sInstance == this)
        {
            sInstance = nullptr;
        }
    }

    CHIP_ERROR SendMessage(const Transport::PeerAddress & address, System::PacketBufferHandle && msgBuf) override
    {
        VerifyOrReturnError(address.GetTransportType() == Type::kFakeOperational, CHIP_ERROR_INVALID_ARGUMENT);
        VerifyOrReturnError(sSendFn != nullptr, CHIP_ERROR_INCORRECT_STATE);
        VerifyOrReturnError(!msgBuf.IsNull(), CHIP_ERROR_INVALID_ARGUMENT);
        // These operational packets are small and single-buffer; no chained handling needed.
        VerifyOrReturnError(!msgBuf->HasChainedBuffer(), CHIP_ERROR_INVALID_MESSAGE_LENGTH);
        sSendFn(msgBuf->Start(), static_cast<size_t>(msgBuf->DataLength()));
        return CHIP_NO_ERROR;
    }

    bool CanSendToPeer(const Transport::PeerAddress & address) override
    {
        return address.GetTransportType() == Type::kFakeOperational;
    }

    /**
     * Deliver a received operational packet up the stack. MUST be called on the
     * Matter/CHIP event-loop thread (platforms marshal onto it before calling).
     */
    void OnMessageReceived(System::PacketBufferHandle && buffer)
    {
        HandleMessageReceived(PeerAddress(Type::kFakeOperational), std::move(buffer));
    }

    /// Register the platform byte-pipe writer. Call once at boot.
    static void SetPlatformSend(FakeOperationalSendFn fn) { sSendFn = fn; }

    /// The fixed synthetic peer address for the single emulated operational peer.
    static PeerAddress PeerAddr() { return PeerAddress(Type::kFakeOperational); }

    static FakeOperational * Instance() { return sInstance; }

private:
    static FakeOperational * sInstance;
    static FakeOperationalSendFn sSendFn;
};

} // namespace Transport
} // namespace chip
