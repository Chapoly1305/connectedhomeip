/*
 *
 * Copyright (c) 2023 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "temperature_measurement.h"

#include <imgui.h>

#include <math.h>

#include <app-common/zap-generated/attributes/Accessors.h>

#include <app-common/zap-generated/cluster-enums.h>

namespace example {
namespace Ui {
namespace Windows {

void TemperatureMeasurement::UpdateState()
{
    if (mTargetLevel.HasValue())
    {
        chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(mEndpointId, mTargetLevel.Value());
        mTargetLevel.ClearValue();
    }

    chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Get(mEndpointId, mCurrentLevel);
    chip::app::Clusters::TemperatureMeasurement::Attributes::MinMeasuredValue::Get(mEndpointId, mMinLevel);
    chip::app::Clusters::TemperatureMeasurement::Attributes::MaxMeasuredValue::Get(mEndpointId, mMaxLevel);
}

void TemperatureMeasurement::Render()
{
    ImGui::Begin(mTitle.c_str());
    ImGui::Text("On Endpoint %d", mEndpointId);
    ImGui::Text("Temperature Measurement:");

    ImGui::Indent();

    if (mMinLevel.IsNull())
    {
        ImGui::Text("MIN Temperature: NULL");
    }
    else
    {
        ImGui::Text("MIN Temperature: %d", mMinLevel.Value());
    }

    if (mMaxLevel.IsNull())
    {
        ImGui::Text("MAX Temperature: NULL");
    }
    else
    {
        ImGui::Text("MAX Temperature: %d", mMaxLevel.Value());
    }

    if (mCurrentLevel.IsNull())
    {
        ImGui::Text("Current Temperature: NULL");
    }
    else
    {
        static int temperature = static_cast<int>(mCurrentLevel.Value());
        ImGui::SliderInt("Target Temperature", &temperature, static_cast<int>(mMinLevel.ValueOr(0)), static_cast<int>(mMaxLevel.ValueOr(0)));

        if (ImGui::Button("Set Temperature"))
        {
            mTargetLevel.SetValue(static_cast<int16_t>(temperature));
        }
    }

    ImGui::Unindent();

    ImGui::End();
}

} // namespace Windows
} // namespace Ui
} // namespace example