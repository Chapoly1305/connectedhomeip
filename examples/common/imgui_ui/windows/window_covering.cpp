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

#include "window_covering.h"

#include <imgui.h>

#include <math.h>

#include <app-common/zap-generated/attributes/Accessors.h>

#include <app-common/zap-generated/cluster-enums.h>

namespace example {
namespace Ui {
namespace Windows {

void WindowCovering::UpdateState()
{
    if (mTargetLevel.HasValue())
    {
        chip::app::Clusters::WindowCovering::Attributes::CurrentPositionLiftPercentage::Set(mEndpointId, mTargetLevel.Value());
        mTargetLevel.ClearValue();
    }

    chip::app::Clusters::WindowCovering::Attributes::CurrentPositionLiftPercentage::Get(mEndpointId, mCurrentLevel);
}

void WindowCovering::Render()
{
    ImGui::Begin(mTitle.c_str());
    ImGui::Text("On Endpoint %d", mEndpointId);
    ImGui::Text("WindowCovering CurrentPositionLiftPercentage:");

    ImGui::Indent();

    if (mCurrentLevel.IsNull())
    {
        ImGui::Text("CurrentPositionLiftPercentage: NULL");
    }
    else
    {
        static int liftValue = static_cast<int>(mCurrentLevel.Value()/100);
        ImGui::SliderInt("Target Value", &liftValue, static_cast<int>(0), static_cast<int>(100));

        if (ImGui::Button("Set Value"))
        {
            mTargetLevel.SetValue(static_cast<int16_t>(liftValue)*100);
        }
    }

    ImGui::Unindent();

    ImGui::End();
}

} // namespace Windows
} // namespace Ui
} // namespace example