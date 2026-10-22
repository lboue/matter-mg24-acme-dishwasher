#include "EnergyManagement.h"
#include "DishwasherManager.h"

#include <DeviceEnergyManagementManager.h>
#include <lib/support/logging/CHIPLogging.h>

#include <memory>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::DeviceEnergyManagement;

namespace {
std::unique_ptr<DeviceEnergyManagementDelegate> gDEMDelegate;
std::unique_ptr<DeviceEnergyManagementManager> gDEMInstance;
DishwasherEnergyDelegate gDishwasherEnergyDelegate;
} // namespace

CHIP_ERROR DishwasherEnergyDelegate::HandleDeviceEnergyManagementStartTimeAdjustRequest(const uint32_t requestedStartTimeUtc,
                                                                                       AdjustmentCauseEnum cause)
{
    ChipLogProgress(AppServer, "StartTime Adjustment received: New start time: %lu, cause %u",
                    static_cast<unsigned long>(requestedStartTimeUtc), static_cast<unsigned>(to_underlying(cause)));

    // The reference delegate has already moved its copy of the forecast. The dishwasher moves its start
    // and replaces that copy with its own forecast, so the forecast ID stays in step with ours.
    return DishwasherMgr().AdjustStartTime(requestedStartTimeUtc, cause);
}

CHIP_ERROR DeviceEnergyManagement::EnergyManagementInit()
{
    BitMask<Feature> features(Feature::kPowerForecastReporting, Feature::kStartTimeAdjustment);

    ReturnErrorOnFailure(DeviceEnergyManagementInit(kEnergyManagementEndpoint, gDEMDelegate, gDEMInstance, features));

    gDEMDelegate->SetDEMManufacturerDelegate(gDishwasherEnergyDelegate);

    TEMPORARY_RETURN_IGNORED gDEMDelegate->SetESAType(ESATypeEnum::kDishwasher);
    TEMPORARY_RETURN_IGNORED gDEMDelegate->SetESACanGenerate(false);
    TEMPORARY_RETURN_IGNORED gDEMDelegate->SetESAState(ESAStateEnum::kOnline);
    TEMPORARY_RETURN_IGNORED gDEMDelegate->SetAbsMinPower(0);
    TEMPORARY_RETURN_IGNORED gDEMDelegate->SetAbsMaxPower(2'000'000); // 2 kW, the heater, the highest power in any program's forecast
    TEMPORARY_RETURN_IGNORED gDEMDelegate->SetOptOutState(OptOutStateEnum::kOptOut);

    return CHIP_NO_ERROR;
}

DeviceEnergyManagementDelegate & DeviceEnergyManagement::GetDEMDelegate()
{
    VerifyOrDie(gDEMDelegate);
    return *gDEMDelegate;
}
