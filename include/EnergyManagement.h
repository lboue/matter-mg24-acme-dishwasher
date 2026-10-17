#pragma once

#include <DEMManufacturerDelegate.h>
#include <DeviceEnergyManagementDelegateImpl.h>

namespace chip {
namespace app {
namespace Clusters {
namespace DeviceEnergyManagement {

/**
 * Glue between the SDK's reference DEM delegate (which owns the Forecast and OptOutState attributes
 * and validates the commands) and the dishwasher. Only Power Forecast Reporting and Start Time
 * Adjustment are enabled, matching the ESP32 version.
 */
class DishwasherEnergyDelegate : public DEMManufacturerDelegate
{
public:
    int64_t GetApproxEnergyDuringSession() override { return 0; }

    CHIP_ERROR HandleDeviceEnergyManagementStartTimeAdjustRequest(const uint32_t requestedStartTimeUtc,
                                                                  AdjustmentCauseEnum cause) override;
};

constexpr EndpointId kEnergyManagementEndpoint = 2;

CHIP_ERROR EnergyManagementInit();

/** Only valid after EnergyManagementInit(). Must be used on the Matter thread. */
DeviceEnergyManagementDelegate & GetDEMDelegate();

} // namespace DeviceEnergyManagement
} // namespace Clusters
} // namespace app
} // namespace chip
