#pragma once
#include "app/util/attribute-storage.h"
#include "locking_service/locking_service.rpc.pb.h"
#include "pigweed/rpc_services/internal/StatusUtils.h"
#include <app-common/zap-generated/attributes/Accessors.h>
#include <platform/PlatformManager.h>

namespace chip {
namespace rpc {

class Locking final : public pw_rpc::nanopb::Locking::Service<Locking>
{
public:
    virtual ~Locking() = default;

    virtual pw::Status Set(const chip_rpc_LockingSetRequest & request, chip_rpc_LockingSetResponse & response)
    {
        EndpointId endpointId = request.endpoint_id;
        uint32_t newState         = request.state_value;

        EventNumber eventNumber;
        {
            DeviceLayer::StackLock lock;
            RETURN_STATUS_IF_NOT_OK(app::Clusters::DoorLock::Attributes::LockState::Set(endpointId, static_cast<chip::app::Clusters::DoorLock::DlLockState>(newState)));
        }

        response.event_number = static_cast<uint64_t>(eventNumber);
        return pw::OkStatus();
    }

    virtual pw::Status Get(const chip_rpc_LockingGetRequest & request, chip_rpc_LockingState & response)
    {
        EndpointId endpointId = request.endpoint_id;
        chip::app::DataModel::Nullable<chip::app::Clusters::DoorLock::DlLockState> state_value;
        {
            DeviceLayer::StackLock lock;
            RETURN_STATUS_IF_NOT_OK(app::Clusters::DoorLock::Attributes::LockState::Get(endpointId, state_value));
        }
        response.state_value = static_cast<uint32_t>(state_value.Value());
        return pw::OkStatus();
    }
};

} // namespace rpc
} // namespace chip
