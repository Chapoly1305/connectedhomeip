/*
 *
 *    Copyright (c) 2023 Project CHIP Authors
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
#include "door_lock.h"

#include <imgui.h>

#include <math.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/cluster-enums.h>

namespace example {
namespace Ui {
namespace Windows {

void DoorLock::UpdateState()
{
    if (mTargetState.HasValue())
    {
        chip::app::Clusters::DoorLock::DlLockState lockState = mTargetState.Value()
            ? chip::app::Clusters::DoorLock::DlLockState::kLocked
            : chip::app::Clusters::DoorLock::DlLockState::kUnlocked;

        chip::app::Clusters::DoorLock::Attributes::LockState::Set(mEndpointId, lockState);
        mTargetState.ClearValue();
    }

    chip::app::Clusters::DoorLock::Attributes::LockState::Get(mEndpointId, mState);
}

void DoorLock::Render()
{
    ImGui::Begin(mTitle.c_str());
    ImGui::Text("On Endpoint %d", mEndpointId);

    bool uiState = mState.ValueOr(chip::app::Clusters::DoorLock::DlLockState::kUnlocked) == chip::app::Clusters::DoorLock::DlLockState::kLocked;
    ImGui::Checkbox("State Value", &uiState);

    if (uiState != (mState.ValueOr(chip::app::Clusters::DoorLock::DlLockState::kUnlocked) == chip::app::Clusters::DoorLock::DlLockState::kLocked))
    {
        mTargetState.SetValue(uiState);
    }

    ImGui::End();
}

} // namespace Windows
} // namespace Ui
} // namespace example
