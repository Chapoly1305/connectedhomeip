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
 *   Software-only CHIPoBLE transport used when running the lighting-app firmware
 *   inside the Renode emulator (no real Silicon Labs Bluetooth stack/radio involved).
 *
 *   Drives the BLEManagerImpl state machine from a simple framed byte stream on
 *   EUSART1 instead of real sl_bt GATT events. The frame format is:
 *     [1 byte type][2 bytes length, little-endian][payload...]
 *   type: 0x01 CONNECT, 0x02 DISCONNECT, 0x03 WRITE_REQUEST (C1),
 *         0x04 SUBSCRIBE, 0x05 UNSUBSCRIBE, 0x06 INDICATION (C2)
 *   The matching host-side implementation lives in
 *   src/platform/Linux/ble/FakeBleTransport.cpp (chip-tool).
 *
 *   Only ever compiled in when CHIP_DEVICE_CONFIG_ENABLE_FAKE_BLE_TRANSPORT=1.
 */

#include <platform/internal/CHIPDeviceLayerInternal.h>
#if CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE && CHIP_DEVICE_CONFIG_ENABLE_FAKE_BLE_TRANSPORT

#include "sl_component_catalog.h"

#include <platform/internal/BLEManager.h>

#include "FreeRTOS.h"
#include "em_eusart.h"
#include "uartdrv.h"
#include <cmsis_os2.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

using namespace chip::Ble;
using namespace chip::System;

namespace chip {
namespace DeviceLayer {
namespace Internal {

namespace {

// Fixed single-connection handle: the fake transport only ever talks to one peer.
constexpr uint8_t kFakeConnHandle = 1;

// Frame type bytes -- must match src/platform/Linux/ble/FakeBleTransport.cpp on the host.
enum class FrameType : uint8_t
{
    kConnect       = 0x01,
    kDisconnect    = 0x02,
    kWriteRequest  = 0x03,
    kSubscribe     = 0x04,
    kUnsubscribe   = 0x05,
    kIndication    = 0x06,
};

constexpr uint16_t kMaxFramePayload = 512;

// Second UARTDRV instance, hand-instantiated (no SLC-generated config): EUSART1 is unused by
// every board this test setup targets, and pin routing is irrelevant since this only ever
// runs against Renode's register-level EUSART model, never real silicon.
DEFINE_BUF_QUEUE(8, sFakeBleRxBuffer);
DEFINE_BUF_QUEUE(8, sFakeBleTxBuffer);
UARTDRV_HandleData_t sFakeBleUartHandleData;
UARTDRV_Handle_t sFakeBleUartHandle = &sFakeBleUartHandleData;

UARTDRV_InitEuart_t sFakeBleUartInit = {
    .port                = EUSART1,
    .useLowFrequencyMode = false,
    .baudRate            = 115200,
    .txPort              = SL_GPIO_PORT_C,
    .rxPort              = SL_GPIO_PORT_C,
    .txPin               = 0,
    .rxPin               = 1,
    .uartNum             = 1,
    .stopBits            = eusartStopbits1,
    .parity              = eusartNoParity,
    .oversampling        = eusartOVS16,
    .mvdis               = eusartMajorityVoteEnable,
    .fcType              = uartdrvFlowControlNone,
    .ctsPort             = SL_GPIO_PORT_C,
    .ctsPin              = 2,
    .rtsPort             = SL_GPIO_PORT_C,
    .rtsPin              = 3,
    .rxQueue             = (UARTDRV_Buffer_FifoQueue_t *) &sFakeBleRxBuffer,
    .txQueue             = (UARTDRV_Buffer_FifoQueue_t *) &sFakeBleTxBuffer,
};

osThreadId_t sFakeBleTaskHandle;
constexpr uint32_t kFakeBleTaskStackSize = 2048;
uint8_t sFakeBleTaskStack[kFakeBleTaskStackSize];
osThread_t sFakeBleTaskControlBlock;
constexpr osThreadAttr_t kFakeBleTaskAttr = {
    .name       = "FakeBLE",
    .attr_bits  = osThreadDetached,
    .cb_mem     = &sFakeBleTaskControlBlock,
    .cb_size    = osThreadCbSize,
    .stack_mem  = sFakeBleTaskStack,
    .stack_size = kFakeBleTaskStackSize,
    // Run ABOVE the Matter/CHIP task (osPriorityHigh) and OpenThread (24) so incoming CHIPoBLE frames
    // -- including the BTP acks chip-tool sends during the crypto-heavy commissioning data phase -- are
    // read promptly instead of being starved by the busy CHIP event loop. Two earlier failure modes:
    //   - too HIGH with a tight spin (original osPriorityRealtime6, NO yield): starved the CHIP task and
    //     PASE stalled at the BLE handshake;
    //   - too LOW (osPriorityLow): the CHIP task starved THIS task during the data phase, so chip-tool's
    //     BTP acks were not consumed in time and the device hit "ack recv timeout" and dropped the link.
    // BlockingRead now yields (osDelay(1)) every poll. Run at the SAME priority as the CHIP task
    // (osPriorityHigh): each runs its bursts and blocks, so they co-schedule -- the CHIP task is not
    // preempted during the tight PASE handshake (which a higher priority broke), and this task still
    // gets serviced whenever the CHIP task is waiting, so chip-tool's data-phase BTP acks are consumed.
    .priority   = osPriorityHigh,
};

// Completion flag for the non-blocking receive below. Set from the UARTDRV DMA-completion
// callback (ISR context), cleared before each new receive. `volatile` because it is written
// from the callback and spun-on from task context.
volatile bool sFakeBleRxComplete = false;

void FakeBleRxCallback(UARTDRV_Handle_t handle, Ecode_t status, uint8_t * data, UARTDRV_Count_t count)
{
    (void) handle;
    (void) status;
    (void) data;
    (void) count;
    sFakeBleRxComplete = true;
}

// Blocking read of exactly `length` bytes -- but implemented on top of the NON-blocking
// UARTDRV_Receive() so the task genuinely SLEEPS (osDelay) while waiting for bytes.
//
// The earlier implementation used UARTDRV_ReceiveB() (the *blocking* variant), on the mistaken
// assumption that under Renode it returns immediately when no bytes are queued. It does NOT:
// ReceiveB starts a DMA and busy-spins internally on the transfer's remaining-count until all
// `length` bytes arrive, never yielding. At osPriorityHigh that spin starves the lower-priority
// OpenThread task (prio 24), so when commissioning reaches ThreadNetworkEnable -- where the
// device must run OpenThread in the background to attach while chip-tool waits silently for the
// ConnectNetwork response -- OpenThread never gets CPU, never attaches, and the command times out.
//
// With the non-blocking Receive + osDelay(1) poll, this task blocks between polls, so OpenThread
// (and the Matter event loop) run freely; incoming frames still preempt promptly (this task stays
// osPriorityHigh) so the EUSART RX FIFO is drained without overflow.
void BlockingRead(uint8_t * buf, uint16_t length)
{
    sFakeBleRxComplete = false;

    while (UARTDRV_Receive(sFakeBleUartHandle, buf, static_cast<UARTDRV_Count_t>(length), FakeBleRxCallback) !=
           ECODE_EMDRV_UARTDRV_OK)
    {
        // Queue momentarily full/busy -- yield and retry.
        osDelay(1);
    }

    while (!sFakeBleRxComplete)
    {
        // Sleep while the DMA fills `buf`. This is the critical yield: it lets OpenThread and the
        // Matter event loop run while no CHIPoBLE bytes are pending.
        osDelay(1);
    }
}

void FakeBLETransportTaskMain(void * arg)
{
    uint8_t header[3];
    uint8_t payload[kMaxFramePayload];

    while (true)
    {
        BlockingRead(header, sizeof(header));
        uint8_t type   = header[0];
        uint16_t plen  = static_cast<uint16_t>(header[1]) | (static_cast<uint16_t>(header[2]) << 8);
        VerifyOrDie(plen <= kMaxFramePayload);
        if (plen > 0)
        {
            BlockingRead(payload, plen);
        }

        BLEMgrImpl().FakeBLEHandleFrame(type, payload, plen);
    }
}

} // namespace

void BLEManagerImpl::InitFakeBLETransport(void)
{
    if (sFakeBleTaskHandle != nullptr)
    {
        return; // already initialized
    }

    UARTDRV_InitEuart(sFakeBleUartHandle, &sFakeBleUartInit);

    sFakeBleTaskHandle = osThreadNew(FakeBLETransportTaskMain, nullptr, &kFakeBleTaskAttr);
    VerifyOrDie(sFakeBleTaskHandle != nullptr);

    ChipLogProgress(DeviceLayer, "FakeBLETransport: listening on EUSART1 for CHIPoBLE frames");
}

void BLEManagerImpl::FakeBLEHandleFrame(uint8_t rawType, const uint8_t * payload, uint16_t len)
{
    FrameType type = static_cast<FrameType>(rawType);
    ChipDeviceEvent event;

    switch (type)
    {
    case FrameType::kConnect: {
        ChipLogProgress(DeviceLayer, "FakeBLETransport: CONNECT");
        AddConnection(kFakeConnHandle, 0);
        TEMPORARY_RETURN_IGNORED PlatformMgr().ScheduleWork(DriveBLEState, 0);
        break;
    }

    case FrameType::kDisconnect: {
        ChipLogProgress(DeviceLayer, "FakeBLETransport: DISCONNECT");
        if (RemoveConnection(kFakeConnHandle))
        {
            event.Type                           = DeviceEventType::kCHIPoBLEConnectionError;
            event.CHIPoBLEConnectionError.ConId  = kFakeConnHandle;
            event.CHIPoBLEConnectionError.Reason = BLE_ERROR_REMOTE_DEVICE_DISCONNECTED;
            PlatformMgr().PostEventOrDie(&event);

            mFlags.Set(Flags::kRestartAdvertising);
            mFlags.Set(Flags::kFastAdvertisingEnabled);
            TEMPORARY_RETURN_IGNORED PlatformMgr().ScheduleWork(DriveBLEState, 0);
        }
        break;
    }

    case FrameType::kWriteRequest: {
        PacketBufferHandle buf = PacketBufferHandle::NewWithData(payload, len, 0, 0);
        VerifyOrReturn(!buf.IsNull(), ChipLogError(DeviceLayer, "FakeBLETransport: OOM on write request"));

        ChipLogDetail(DeviceLayer, "FakeBLETransport: WRITE_REQUEST len=%u", len);
        event.Type                       = DeviceEventType::kCHIPoBLEWriteReceived;
        event.CHIPoBLEWriteReceived.ConId = kFakeConnHandle;
        event.CHIPoBLEWriteReceived.Data  = std::move(buf).UnsafeRelease();
        CHIP_ERROR err                    = PlatformMgr().PostEvent(&event);
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "FakeBLETransport: PostEvent(WriteReceived) failed: %" CHIP_ERROR_FORMAT, err.Format());
        }
        break;
    }

    case FrameType::kSubscribe:
    case FrameType::kUnsubscribe: {
        bool subscribe               = (type == FrameType::kSubscribe);
        CHIPoBLEConState * connState = GetConnectionState(kFakeConnHandle);
        VerifyOrReturn(connState != nullptr,
                        ChipLogError(DeviceLayer, "FakeBLETransport: %s with no active connection", subscribe ? "SUBSCRIBE" : "UNSUBSCRIBE"));

        ChipLogDetail(DeviceLayer, "FakeBLETransport: %s", subscribe ? "SUBSCRIBE" : "UNSUBSCRIBE");
        connState->subscribed          = subscribe ? 1 : 0;
        event.Type                     = subscribe ? DeviceEventType::kCHIPoBLESubscribe : DeviceEventType::kCHIPoBLEUnsubscribe;
        event.CHIPoBLESubscribe.ConId  = kFakeConnHandle;
        CHIP_ERROR err                 = PlatformMgr().PostEvent(&event);
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "FakeBLETransport: PostEvent(Subscribe) failed: %" CHIP_ERROR_FORMAT, err.Format());
        }
        break;
    }

    default:
        ChipLogError(DeviceLayer, "FakeBLETransport: unknown frame type 0x%02x", rawType);
        break;
    }
}

CHIP_ERROR BLEManagerImpl::SendFakeIndication(BLE_CONNECTION_OBJECT conId, const ChipBleUUID * svcId, const ChipBleUUID * charId,
                                              PacketBufferHandle data)
{
    CHIPoBLEConState * conState = GetConnectionState(conId);
    VerifyOrReturnError((conState != nullptr) && (conState->subscribed != 0), CHIP_ERROR_INVALID_ARGUMENT);

    uint8_t header[3];
    header[0] = static_cast<uint8_t>(FrameType::kIndication);
    header[1] = static_cast<uint8_t>(data->DataLength() & 0xFF);
    header[2] = static_cast<uint8_t>((data->DataLength() >> 8) & 0xFF);

    UARTDRV_ForceTransmit(sFakeBleUartHandle, header, sizeof(header));
    UARTDRV_ForceTransmit(sFakeBleUartHandle, data->Start(), static_cast<UARTDRV_Count_t>(data->DataLength()));

    // The fake transport is a reliable in-order pipe, so acknowledge the indication immediately
    // instead of waiting on a real ATT confirmation round-trip.
    HandleTxConfirmationEvent(conId);

    return CHIP_NO_ERROR;
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip

#endif // CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE && CHIP_DEVICE_CONFIG_ENABLE_FAKE_BLE_TRANSPORT
