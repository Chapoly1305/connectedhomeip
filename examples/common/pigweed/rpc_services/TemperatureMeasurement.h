#pragma once

#include <app-common/zap-generated/attributes/Accessors.h>
#include <platform/PlatformManager.h>

#include "app/util/attribute-storage.h"
#include "temperature_service/temperature_service.rpc.pb.h"
#include "pigweed/rpc_services/internal/StatusUtils.h"

namespace chip {
namespace rpc {

class Temperature final : public pw_rpc::nanopb::Temperature::Service<Temperature>
{
 public:
  virtual ~Temperature() = default;

  virtual pw::Status Set(
      const chip_rpc_TemperatureSetRequest& request,
      chip_rpc_TemperatureSetResponse& response)
  {
    EndpointId endpointId = request.endpoint_id;
    int16_t newTemperature = request.state_value;
    EventNumber eventNumber;

    {
      DeviceLayer::StackLock lock;
      RETURN_STATUS_IF_NOT_OK(
          app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(
              endpointId, newTemperature));
    }

    response.event_number = static_cast<uint64_t>(eventNumber);
    return pw::OkStatus();
  }

  virtual pw::Status Get(
      const chip_rpc_TemperatureGetRequest& request,
      chip_rpc_TemperatureState& response)
  {
    EndpointId endpointId = request.endpoint_id;
    chip::app::DataModel::Nullable<int16_t> temperature_value;

    {
      DeviceLayer::StackLock lock;
      RETURN_STATUS_IF_NOT_OK(
          app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Get(
              endpointId, temperature_value));
    }

    response.state_value = temperature_value.Value();
    return pw::OkStatus();
  }
};

}  // namespace rpc
}  // namespace chip