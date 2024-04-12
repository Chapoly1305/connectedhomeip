/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
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

#include <AppMain.h>
#include <app/util/af.h>
#include <platform/CHIPDeviceConfig.h>
#include "LightingAppCommandDelegate.h"
#include "LightingManager.h"
#include "WindowCoveringManager.h"
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <app/server/Server.h>
#include <lib/support/logging/CHIPLogging.h>

#if defined(CHIP_IMGUI_ENABLED) && CHIP_IMGUI_ENABLED
#include <imgui_ui/ui.h>
#include <imgui_ui/windows/temperature_measurement.h>
#include <imgui_ui/windows/boolean_state.h>
#include <imgui_ui/windows/occupancy_sensing.h>
#include <imgui_ui/windows/qrcode.h>
#include <imgui_ui/windows/light.h>
#include <imgui_ui/windows/window_covering.h>
#include <imgui_ui/windows/door_lock.h>
#endif

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

namespace {

constexpr char kChipEventFifoPathPrefix[] = "/tmp/combine_fifo_";
NamedPipeCommands sChipNamedPipeCommands;
LightingAppCommandDelegate sLightingAppCommandDelegate;
Clusters::WindowCovering::WindowCoveringManager sWindowCoveringManager;

} // namespace

void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath & attributePath, uint8_t type, uint16_t size,
                                       uint8_t * value)
{
    if (attributePath.mClusterId == OnOff::Id && attributePath.mAttributeId == OnOff::Attributes::OnOff::Id)
    {
        LightingMgr().InitiateAction(*value ? LightingManager::ON_ACTION : LightingManager::OFF_ACTION);
    }
}

void emberAfOnOffClusterInitCallback(EndpointId endpoint)
{
    // TODO: implement any additional Cluster Server init actions
}

void emberAfWindowCoveringClusterInitCallback(chip::EndpointId endpoint)
{
    sWindowCoveringManager.Init(endpoint);
    Clusters::WindowCovering::SetDefaultDelegate(endpoint, &sWindowCoveringManager);
    Clusters::WindowCovering::ConfigStatusUpdateFeatures(endpoint);
}

void ApplicationInit()
{
    std::string path = kChipEventFifoPathPrefix + std::to_string(getpid());

    if (sChipNamedPipeCommands.Start(path, &sLightingAppCommandDelegate) != CHIP_NO_ERROR)
    {
        ChipLogError(NotSpecified, "Failed to start CHIP NamedPipeCommands");
        sChipNamedPipeCommands.Stop();
    }
}

void ApplicationShutdown()
{
    if (sChipNamedPipeCommands.Stop() != CHIP_NO_ERROR)
    {
        ChipLogError(NotSpecified, "Failed to stop CHIP NamedPipeCommands");
    }
}

int main(int argc, char * argv[])
{
    VerifyOrDie(ChipLinuxAppInit(argc, argv) == 0);
    CHIP_ERROR err = LightingMgr().Init();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(AppServer, "Failed to initialize lighting manager: %" CHIP_ERROR_FORMAT, err.Format());
        chip::DeviceLayer::PlatformMgr().Shutdown();
        return -1;
    }

    #if defined(CHIP_IMGUI_ENABLED) && CHIP_IMGUI_ENABLED
        example::Ui::ImguiUi ui;

        ui.AddWindow(std::make_unique<example::Ui::Windows::QRCode>());
        ui.AddWindow(std::make_unique<example::Ui::Windows::OccupancySensing>(chip::EndpointId(2), "Occupancy"));
        ui.AddWindow(std::make_unique<example::Ui::Windows::Light>(chip::EndpointId(1)));
        ui.AddWindow(std::make_unique<example::Ui::Windows::WindowCovering>(chip::EndpointId(3), "WindowCovering"));
        ui.AddWindow(std::make_unique<example::Ui::Windows::BooleanState>(chip::EndpointId(4), "Contact Sensor"));
        ui.AddWindow(std::make_unique<example::Ui::Windows::TemperatureMeasurement>(chip::EndpointId(5), "Temperature"));
        ui.AddWindow(std::make_unique<example::Ui::Windows::DoorLock>(chip::EndpointId(6), "DoorLock"));



        ChipLinuxAppMainLoop(&ui);
    #else
        ChipLinuxAppMainLoop();
    #endif

        return 0;
}
