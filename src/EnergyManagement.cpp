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
    ChipLogProgress(AppServer, "StartTime Adjustment received: New start time: %lu",
                    static_cast<unsigned long>(requestedStartTimeUtc));

    // The reference delegate has already moved the forecast; we just need to move our own start.
    return DishwasherMgr().AdjustStartTime(requestedStartTimeUtc);
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
