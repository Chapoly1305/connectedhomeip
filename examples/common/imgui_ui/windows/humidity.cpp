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

#include "humidity.h"

#include <imgui.h>

#include <math.h>

#include <app-common/zap-generated/attributes/Accessors.h>

#include <app-common/zap-generated/cluster-enums.h>

namespace example {
namespace Ui {
namespace Windows {

void RelativeHumidityMeasurement::UpdateState()
{
    if (mTargetLevel.HasValue())
    {
        chip::app::Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Set(mEndpointId, mTargetLevel.Value()*100);
        mTargetLevel.ClearValue();
    }

    chip::app::Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Get(mEndpointId, mCurrentLevel);
    chip::app::Clusters::RelativeHumidityMeasurement::Attributes::MinMeasuredValue::Get(mEndpointId, mMinLevel);
    chip::app::Clusters::RelativeHumidityMeasurement::Attributes::MaxMeasuredValue::Get(mEndpointId, mMaxLevel);
}

void RelativeHumidityMeasurement::Render()
{
    ImGui::Begin(mTitle.c_str());
    ImGui::Text("On Endpoint %d", mEndpointId);
    ImGui::Text("Humidity Measurement:");

    ImGui::Indent();

    if (mMinLevel.IsNull())
    {
        ImGui::Text("MIN Humidity: NULL");
    }
    else
    {
        ImGui::Text("MIN Humidity: %d", mMinLevel.Value());
    }

    if (mMaxLevel.IsNull())
    {
        ImGui::Text("MAX Humidity: NULL");
    }
    else
    {
        ImGui::Text("MAX Humidity: %d", mMaxLevel.Value());
    }

    if (mCurrentLevel.IsNull())
    {
        ImGui::Text("Current Humidity: NULL");
    }
    else
    {
        static int Humidity = static_cast<int>(mCurrentLevel.Value()/100);
        ImGui::SliderInt("Target Humidity", &Humidity, static_cast<int>(mMinLevel.ValueOr(0)/100), static_cast<int>(mMaxLevel.ValueOr(10000)/100));

        if (ImGui::Button("Set Humidity"))
        {
            mTargetLevel.SetValue(static_cast<int16_t>(Humidity)*100);
        }
    }

    ImGui::Unindent();

    ImGui::End();
}

} // namespace Windows
} // namespace Ui
} // namespace example