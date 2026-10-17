/*
 * Matter cluster delegates for the dishwasher endpoint.
 *
 * Ported from app_driver.cpp in matter-esp32-tiny-dishwasher. Everything in here runs on the
 * Matter thread, so it can call straight into the DishwasherManager.
 */

#include "DishwasherDelegates.h"
#include "DishwasherManager.h"

#include <app/util/generic-callbacks.h>
#include <lib/support/logging/CHIPLogging.h>

#include <memory>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using chip::Protocols::InteractionModel::Status;

//*******************************
//* OPERATIONAL STATE DELEGATE *
//*******************************

namespace {
std::unique_ptr<OperationalState::OperationalStateDelegate> gOperationalStateDelegate;
std::unique_ptr<OperationalState::Instance> gOperationalStateInstance;
} // namespace

using OperationalState::ErrorStateEnum;
using OperationalState::GenericOperationalError;
using OperationalState::GenericOperationalState;
using OperationalState::OperationalStateDelegate;

DataModel::Nullable<uint32_t> OperationalStateDelegate::GetCountdownTime()
{
    return DishwasherMgr().GetCountdownTime();
}

CHIP_ERROR OperationalStateDelegate::GetOperationalStateAtIndex(size_t index, GenericOperationalState & operationalState)
{
    VerifyOrReturnError(index < mOperationalStateList.size(), CHIP_ERROR_NOT_FOUND);
    operationalState = mOperationalStateList[index];
    return CHIP_NO_ERROR;
}

CHIP_ERROR OperationalStateDelegate::GetOperationalPhaseAtIndex(size_t index, MutableCharSpan & operationalPhase)
{
    VerifyOrReturnError(index < mOperationalPhaseList.size(), CHIP_ERROR_NOT_FOUND);
    return CopyCharSpanToMutableCharSpan(mOperationalPhaseList[index], operationalPhase);
}

// Dead Front: while the dishwasher is off it ignores everything except On/Off.
static bool RejectIfPoweredOff(GenericOperationalError & err)
{
    if (DishwasherMgr().IsPoweredOn())
    {
        return false;
    }
    ChipLogProgress(AppServer, "Dishwasher is off, rejecting command");
    err.Set(to_underlying(ErrorStateEnum::kCommandInvalidInState));
    return true;
}

void OperationalStateDelegate::HandlePauseStateCallback(GenericOperationalError & err)
{
    ChipLogProgress(AppServer, "HandlePauseStateCallback");
    VerifyOrReturn(!RejectIfPoweredOff(err));
    DishwasherMgr().PauseProgram();
    err.Set(to_underlying(ErrorStateEnum::kNoError));
}

void OperationalStateDelegate::HandleResumeStateCallback(GenericOperationalError & err)
{
    ChipLogProgress(AppServer, "HandleResumeStateCallback");
    VerifyOrReturn(!RejectIfPoweredOff(err));
    DishwasherMgr().ResumeProgram();
    err.Set(to_underlying(ErrorStateEnum::kNoError));
}

void OperationalStateDelegate::HandleStartStateCallback(GenericOperationalError & err)
{
    ChipLogProgress(AppServer, "HandleStartStateCallback");
    VerifyOrReturn(!RejectIfPoweredOff(err));
    DishwasherMgr().StartProgram();
    err.Set(to_underlying(ErrorStateEnum::kNoError));
}

void OperationalStateDelegate::HandleStopStateCallback(GenericOperationalError & err)
{
    ChipLogProgress(AppServer, "HandleStopStateCallback");
    DishwasherMgr().StopProgram();
    err.Set(to_underlying(ErrorStateEnum::kNoError));
}

OperationalState::Instance * OperationalState::GetInstance()
{
    return gOperationalStateInstance.get();
}

OperationalStateDelegate * OperationalState::GetDelegate()
{
    return gOperationalStateDelegate.get();
}

void emberAfOperationalStateClusterInitCallback(EndpointId endpointId)
{
    ChipLogProgress(AppServer, "emberAfOperationalStateClusterInitCallback(%u)", endpointId);

    VerifyOrDie(endpointId == DishwasherManager::kDishwasherEndpoint);
    VerifyOrDie(!gOperationalStateDelegate && !gOperationalStateInstance);

    gOperationalStateDelegate = std::make_unique<OperationalStateDelegate>();
    gOperationalStateInstance = std::make_unique<OperationalState::Instance>(gOperationalStateDelegate.get(), endpointId);

    VerifyOrDie(gOperationalStateInstance->Init() == CHIP_NO_ERROR);

    TEMPORARY_RETURN_IGNORED gOperationalStateInstance->SetOperationalState(
        to_underlying(OperationalState::OperationalStateEnum::kStopped));
    TEMPORARY_RETURN_IGNORED gOperationalStateInstance->SetCurrentPhase(DataModel::MakeNullable<uint8_t>(0));
}

//****************************
//* DISHWASHER MODE DELEGATE *
//****************************

namespace {
std::unique_ptr<DishwasherMode::DishwasherModeDelegate> gDishwasherModeDelegate;
std::unique_ptr<ModeBase::Instance> gDishwasherModeInstance;
} // namespace

using DishwasherMode::DishwasherModeDelegate;
template <typename T>
using List              = DataModel::List<T>;
using ModeTagStructType = chip::app::Clusters::detail::Structs::ModeTagStruct::Type;

CHIP_ERROR DishwasherModeDelegate::Init()
{
    return CHIP_NO_ERROR;
}

void DishwasherModeDelegate::HandleChangeToMode(uint8_t newMode, ModeBase::Commands::ChangeToModeResponse::Type & response)
{
    ChipLogProgress(AppServer, "DishwasherModeDelegate::HandleChangeToMode(%u)", newMode);

    // The mode is the program; it can only be changed while nothing is running.
    if (DishwasherMgr().GetOperationalState() != OperationalState::OperationalStateEnum::kStopped)
    {
        response.status = to_underlying(ModeBase::StatusCode::kInvalidInMode);
        response.statusText.SetValue("Program is running"_span);
        return;
    }

    DishwasherMgr().UpdateMode(newMode);
    response.status = to_underlying(ModeBase::StatusCode::kSuccess);
}

CHIP_ERROR DishwasherModeDelegate::GetModeLabelByIndex(uint8_t modeIndex, MutableCharSpan & label)
{
    VerifyOrReturnError(modeIndex < MATTER_ARRAY_SIZE(kModeOptions), CHIP_ERROR_PROVIDER_LIST_EXHAUSTED);
    return CopyCharSpanToMutableCharSpan(kModeOptions[modeIndex].label, label);
}

CHIP_ERROR DishwasherModeDelegate::GetModeValueByIndex(uint8_t modeIndex, uint8_t & value)
{
    VerifyOrReturnError(modeIndex < MATTER_ARRAY_SIZE(kModeOptions), CHIP_ERROR_PROVIDER_LIST_EXHAUSTED);
    value = kModeOptions[modeIndex].mode;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DishwasherModeDelegate::GetModeTagsByIndex(uint8_t modeIndex, List<ModeTagStructType> & tags)
{
    VerifyOrReturnError(modeIndex < MATTER_ARRAY_SIZE(kModeOptions), CHIP_ERROR_PROVIDER_LIST_EXHAUSTED);
    VerifyOrReturnError(tags.size() >= kModeOptions[modeIndex].modeTags.size(), CHIP_ERROR_INVALID_ARGUMENT);

    std::copy(kModeOptions[modeIndex].modeTags.begin(), kModeOptions[modeIndex].modeTags.end(), tags.begin());
    tags.reduce_size(kModeOptions[modeIndex].modeTags.size());

    return CHIP_NO_ERROR;
}

ModeBase::Instance * DishwasherMode::GetInstance()
{
    return gDishwasherModeInstance.get();
}

DishwasherModeDelegate * DishwasherMode::GetDelegate()
{
    return gDishwasherModeDelegate.get();
}

void emberAfDishwasherModeClusterInitCallback(EndpointId endpointId)
{
    ChipLogProgress(AppServer, "emberAfDishwasherModeClusterInitCallback(%u)", endpointId);

    VerifyOrDie(endpointId == DishwasherManager::kDishwasherEndpoint);
    VerifyOrDie(!gDishwasherModeDelegate && !gDishwasherModeInstance);

    gDishwasherModeDelegate = std::make_unique<DishwasherModeDelegate>();
    // TODO Restore the deadfront support by setting the OnOff feature.
    gDishwasherModeInstance = std::make_unique<ModeBase::Instance>(gDishwasherModeDelegate.get(), endpointId, DishwasherMode::Id, 0);

    VerifyOrDie(gDishwasherModeInstance->Init() == CHIP_NO_ERROR);

    ChipLogProgress(AppServer, "CurrentMode: %u", gDishwasherModeInstance->GetCurrentMode());
}
