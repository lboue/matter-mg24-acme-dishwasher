/*
 * Ported from dishwasher_manager.cpp in matter-esp32-tiny-dishwasher.
 *
 * All methods run on the Matter thread (see DishwasherManager.h), so cluster instances are
 * updated directly rather than via ScheduleWork.
 */

#include "DishwasherManager.h"

#include "DishwasherDelegates.h"
#include "EnergyManagement.h"
#include "GridConditionsSubscriber.h"
#include "PriceSubscriber.h"
#include "StartPlanner.h"
#include "StatusDisplay.h"
#include "WallClock.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::OperationalState;

DishwasherManager DishwasherManager::sDishwasher;

namespace {
constexpr System::Clock::Seconds32 kTickInterval(1);

// Start in one minute to allow for optimisation when opted into energy management.
constexpr uint32_t kOptimisationDelayS = 60;

constexpr uint32_t kSlotDurationS = 30 * 60;
constexpr int64_t kSlotPowerMw    = 3'000'000;

// How far ahead a program may be moved, matching the forecast's latestEndTime.
constexpr uint32_t kStartHorizonS = 86400;

using Screen = StatusDisplay::Screen;

// The current period plus the forecast, for StartPlanner. Only used on the Matter thread.
StartPlanner::Period sPlannerPeriods[1 + PriceSubscriber::kMaxForecastEntries];
static_assert(PriceSubscriber::kMaxForecastEntries >= GridConditionsSubscriber::kMaxForecastEntries);

// Track this separately as the forecast struct only holds a list view onto the slots.
DeviceEnergyManagement::Structs::SlotStruct::Type sSlots[3];
DeviceEnergyManagement::Structs::ForecastStruct::Type sForecastStruct;
} // namespace

CHIP_ERROR DishwasherManager::Init()
{
    ChipLogProgress(AppServer, "Initializing DishwasherManager");

    StatusDisplay::Instance().Init();

    bool onOff = false;
    if (OnOff::Attributes::OnOff::Get(kDishwasherEndpoint, &onOff) == Protocols::InteractionModel::Status::Success && onOff)
    {
        TurnOnPower();
    }

    return DeviceLayer::SystemLayer().StartTimer(kTickInterval, ProgramTick, this);
}

void DishwasherManager::ProgramTick(System::Layer * layer, void * context)
{
    DishwasherMgr().ProgressProgram();
    TEMPORARY_RETURN_IGNORED layer->StartTimer(kTickInterval, ProgramTick, context);
}

//************
//* CONTROLS *
//************

void DishwasherManager::HandlePowerButton()
{
    // The On/Off attribute is the source of truth. Writing it calls back into TurnOnPower()/TurnOffPower()
    // via MatterPostAttributeChangeCallback, exactly as a Matter On/Off command would.
    OnOff::Attributes::OnOff::Set(kDishwasherEndpoint, !mIsPoweredOn);
}

void DishwasherManager::HandleStartButton()
{
    if (!mIsPoweredOn)
    {
        ChipLogProgress(AppServer, "Dishwasher is off, cannot handle start");
        return;
    }

    ToggleProgram();
}

void DishwasherManager::HandleCancel()
{
    if (!mIsPoweredOn)
    {
        ChipLogProgress(AppServer, "Dishwasher is off, cannot cancel");
        return;
    }

    if (mIsProgramSelected)
    {
        StopProgram();
    }
}

void DishwasherManager::SelectMode(uint8_t mode)
{
    if (!mIsPoweredOn)
    {
        ChipLogProgress(AppServer, "Dishwasher is off, cannot change mode");
        return;
    }

    if (mIsProgramSelected)
    {
        ChipLogProgress(AppServer, "Mode can only be changed when dishwasher is stopped!");
        return;
    }

    if (DishwasherMode::GetInstance()->UpdateCurrentMode(mode) != Protocols::InteractionModel::Status::Success)
    {
        ChipLogError(AppServer, "Unsupported mode %u", mode);
        return;
    }

    ChipLogProgress(AppServer, "Selected Mode: %u", mode);
    UpdateMode(mode);
}

void DishwasherManager::SelectNextMode()
{
    SelectMode(static_cast<uint8_t>((mMode + 1) % DishwasherMode::DishwasherModeDelegate::kModeCount));
}

void DishwasherManager::HandleWheelTurned(int8_t direction)
{
    if (!mIsPoweredOn)
    {
        return;
    }

    if (mScreen == Screen::kEnergyMenu)
    {
        // Opt Out is on the left of the menu and Opt In on the right.
        SetEnergyManagementOptIn(direction > 0);
        return;
    }

    if (mScreen == Screen::kStartChoice)
    {
        const int next = mSelectedStartChoice + direction;
        if (next >= 0 && next < mStartChoiceCount)
        {
            mSelectedStartChoice = static_cast<uint8_t>(next);
            UpdateDishwasherDisplay();
        }
        return;
    }

    if (mIsProgramSelected)
    {
        return;
    }

    // The programs, then Settings.
    constexpr uint8_t modeCount = DishwasherMode::DishwasherModeDelegate::kModeCount;
    constexpr uint8_t itemCount = modeCount + 1;
    const uint8_t current       = mIsSettingsSelected ? modeCount : mMode;
    const uint8_t next          = static_cast<uint8_t>((current + itemCount + direction) % itemCount);

    if (next == modeCount)
    {
        mIsSettingsSelected = true;
        UpdateDishwasherDisplay();
    }
    else if (next == mMode)
    {
        mIsSettingsSelected = false;
        UpdateDishwasherDisplay();
    }
    else
    {
        SelectMode(next);
    }
}

void DishwasherManager::HandleWheelClicked()
{
    if (!mIsPoweredOn)
    {
        ChipLogProgress(AppServer, "Dishwasher is off, cannot handle wheel click");
        return;
    }

    if (mScreen == Screen::kEnergyMenu)
    {
        mScreen = Screen::kStatus;
        UpdateDishwasherDisplay();
        return;
    }

    if (mScreen == Screen::kStartChoice)
    {
        StartProgram(mStartOptions[mSelectedStartChoice], mStartChoices[mSelectedStartChoice].delayS);
        return;
    }

    if (mIsProgramSelected)
    {
        if (mDelayedStartTimeRemaining > 0)
        {
            ChipLogProgress(AppServer, "Waiting to start; hold the knob to cancel");
        }
        else if (mState == OperationalStateEnum::kRunning)
        {
            PauseProgram();
        }
        else if (mState == OperationalStateEnum::kPaused)
        {
            ResumeProgram();
        }
        return;
    }

    if (mIsSettingsSelected)
    {
        mScreen = Screen::kEnergyMenu;
        UpdateDishwasherDisplay();
        return;
    }

    OpenStartChoice();
}

void DishwasherManager::HandleWheelLongPressed()
{
    if (!mIsPoweredOn)
    {
        return;
    }

    if (mScreen != Screen::kStatus)
    {
        mScreen = Screen::kStatus;
        UpdateDishwasherDisplay();
        return;
    }

    if (mIsProgramSelected)
    {
        StopProgram();
    }
}

void DishwasherManager::OpenStartChoice()
{
    mStartChoiceCount    = 0;
    mSelectedStartChoice = 0;

    auto addChoice = [this](StartOption option, const char * label, uint32_t delayS) {
        mStartOptions[mStartChoiceCount] = option;
        mStartChoices[mStartChoiceCount] = { label, delayS };
        mStartChoiceCount++;
    };

    addChoice(StartOption::kNow, "Now", 0);

    uint32_t delayS = 0;
    if (FindCheapestStart(delayS))
    {
        addChoice(StartOption::kCheapest, "Cheapest", delayS);
    }
    if (FindGreenestStart(delayS))
    {
        addChoice(StartOption::kGreenest, "Greenest", delayS);
    }

    // Nothing to choose between.
    if (mStartChoiceCount == 1)
    {
        StartProgram(StartOption::kNow, 0);
        return;
    }

    mScreen = Screen::kStartChoice;
    UpdateDishwasherDisplay();
}

uint32_t DishwasherManager::GetProgramDuration() const
{
    // 30, 60 or 90 minutes.
    return kSlotDurationS + (mMode * kSlotDurationS);
}

bool DishwasherManager::FindCheapestStart(uint32_t & delayS) const
{
    const auto & prices = PriceSubscriber::Instance();
    uint32_t now        = 0;
    if (!prices.IsForecastSupported() || prices.GetForecast().empty() || !WallClock::Instance().GetMatterEpochSeconds(now))
    {
        return false;
    }

    size_t count = 0;
    auto add     = [&count](const PriceSubscriber::PriceInfo & info) {
        if (info.hasPrice)
        {
            sPlannerPeriods[count++] = { info.periodStart, info.periodEnd, info.price };
        }
    };

    PriceSubscriber::PriceInfo current;
    if (prices.GetCurrentPrice(current))
    {
        add(current);
    }
    for (const auto & entry : prices.GetForecast())
    {
        add(entry);
    }

    return StartPlanner::FindBestStart(Span<const StartPlanner::Period>(sPlannerPeriods, count), now, GetProgramDuration(),
                                       kStartHorizonS, delayS);
}

bool DishwasherManager::FindGreenestStart(uint32_t & delayS) const
{
    const auto & grid = GridConditionsSubscriber::Instance();
    uint32_t now      = 0;
    if (!grid.IsForecastSupported() || grid.GetForecast().empty() || !WallClock::Instance().GetMatterEpochSeconds(now))
    {
        return false;
    }

    size_t count = 0;
    auto add     = [&count](const GridConditionsSubscriber::Conditions & conditions) {
        sPlannerPeriods[count++] = { conditions.periodStart, conditions.periodEnd, conditions.gridCarbonIntensity };
    };

    GridConditionsSubscriber::Conditions current;
    if (grid.GetCurrentConditions(current))
    {
        add(current);
    }
    for (const auto & entry : grid.GetForecast())
    {
        add(entry);
    }

    return StartPlanner::FindBestStart(Span<const StartPlanner::Period>(sPlannerPeriods, count), now, GetProgramDuration(),
                                       kStartHorizonS, delayS);
}

void DishwasherManager::SetEnergyManagementOptIn(bool optIn)
{
    mOptedIntoEnergyManagement = optIn;

    TEMPORARY_RETURN_IGNORED DeviceEnergyManagement::GetDEMDelegate().SetOptOutState(
        optIn ? DeviceEnergyManagement::OptOutStateEnum::kNoOptOut : DeviceEnergyManagement::OptOutStateEnum::kOptOut);

    ChipLogProgress(AppServer, "Opted into energy management: %d", mOptedIntoEnergyManagement);
    UpdateDishwasherDisplay();
}

//*********
//* POWER *
//*********

void DishwasherManager::TurnOnPower()
{
    mIsPoweredOn = true;
    StatusDisplay::Instance().TurnOn();
    UpdateDishwasherDisplay();
}

void DishwasherManager::TurnOffPower()
{
    mIsPoweredOn = false;
    mScreen      = Screen::kStatus;
    StopProgram();
    StatusDisplay::Instance().TurnOff();
}

//***********
//* PROGRAM *
//***********

void DishwasherManager::ToggleProgram()
{
    if (mState == OperationalStateEnum::kStopped)
    {
        StartProgram();
    }
    else if (mState == OperationalStateEnum::kRunning)
    {
        PauseProgram();
    }
    else if (mState == OperationalStateEnum::kPaused)
    {
        ResumeProgram();
    }
}

void DishwasherManager::StartProgram()
{
    BeginProgram(mOptedIntoEnergyManagement ? kOptimisationDelayS : 0, mOptedIntoEnergyManagement);
}

void DishwasherManager::StartProgram(StartOption option, uint32_t delayS)
{
    if (option == StartOption::kNow)
    {
        // Now means now: no window for an energy manager to move it in.
        BeginProgram(0, false);
        return;
    }

    ChipLogProgress(AppServer, "Starting at the %s time, in %lus", option == StartOption::kCheapest ? "cheapest" : "greenest",
                    static_cast<unsigned long>(delayS));
    BeginProgram(delayS, mOptedIntoEnergyManagement);
}

void DishwasherManager::BeginProgram(uint32_t delayS, bool canMoveStart)
{
    if (mIsProgramSelected)
    {
        ChipLogProgress(AppServer, "A program has already been started");
        return;
    }

    mIsProgramSelected  = true;
    mIsSettingsSelected = false;
    mScreen             = Screen::kStatus;
    mCanMoveStart       = canMoveStart;

    mRunningTimeRemaining      = GetProgramDuration();
    mPhase                     = 0;
    mDelayedStartTimeRemaining = delayS;

    SetForecast();
    UpdateDishwasherDisplay();
}

CHIP_ERROR DishwasherManager::AdjustStartTime(uint32_t newStartTime)
{
    VerifyOrReturnError(mOptedIntoEnergyManagement, CHIP_ERROR_INCORRECT_STATE,
                        ChipLogError(AppServer, "Not opted into energy management, ignoring start time adjustment"));
    VerifyOrReturnError(mCanMoveStart, CHIP_ERROR_INCORRECT_STATE,
                        ChipLogError(AppServer, "Program was started now, ignoring start time adjustment"));

    // Once the program has started we can't move it.
    VerifyOrReturnError(mIsProgramSelected && mState == OperationalStateEnum::kStopped && mDelayedStartTimeRemaining > 0,
                        CHIP_ERROR_INCORRECT_STATE, ChipLogError(AppServer, "Program already started, cannot adjust start time"));

    uint32_t now = 0;
    VerifyOrReturnError(WallClock::Instance().GetMatterEpochSeconds(now), CHIP_ERROR_INCORRECT_STATE,
                        ChipLogError(AppServer, "Time not synchronised, cannot adjust start time"));

    mDelayedStartTimeRemaining = newStartTime > now ? newStartTime - now : 0;
    ChipLogProgress(AppServer, "Program will now start in %lus", static_cast<unsigned long>(mDelayedStartTimeRemaining));

    UpdateDishwasherDisplay();
    return CHIP_NO_ERROR;
}

void DishwasherManager::PauseProgram()
{
    if (mState == OperationalStateEnum::kRunning)
    {
        UpdateOperationState(OperationalStateEnum::kPaused);
    }
}

void DishwasherManager::ResumeProgram()
{
    if (mState == OperationalStateEnum::kPaused)
    {
        UpdateOperationState(OperationalStateEnum::kRunning);
    }
}

void DishwasherManager::StopProgram()
{
    mIsProgramSelected         = false;
    mCanMoveStart              = false;
    mDelayedStartTimeRemaining = 0;
    mRunningTimeRemaining      = 0;
    UpdateCurrentPhase(0);

    // Back to the default program, and keep the DishwasherMode cluster in step.
    if (mMode != DishwasherMode::ModeNormal && DishwasherMode::GetInstance() != nullptr)
    {
        TEMPORARY_RETURN_IGNORED DishwasherMode::GetInstance()->UpdateCurrentMode(DishwasherMode::ModeNormal);
    }
    UpdateMode(DishwasherMode::ModeNormal);

    UpdateOperationState(OperationalStateEnum::kStopped);
    ClearForecast();
}

void DishwasherManager::EndProgram()
{
    OperationalState::GetInstance()->OnOperationCompletionDetected(to_underlying(ErrorStateEnum::kNoError));
    StopProgram();
}

void DishwasherManager::ProgressProgram()
{
    // If there is no program selected, we do nothing.
    if (!mIsProgramSelected)
    {
        return;
    }

    // We might be on a delayed start, so tick down if required.
    if (mDelayedStartTimeRemaining > 0)
    {
        mDelayedStartTimeRemaining--;
        OperationalState::GetInstance()->UpdateCountdownTimeFromDelegate();
        UpdateDishwasherDisplay();
        return;
    }

    // If we are stopped, we should start running.
    if (mState == OperationalStateEnum::kStopped)
    {
        UpdateOperationState(OperationalStateEnum::kRunning);
    }

    if (mState != OperationalStateEnum::kRunning)
    {
        return;
    }

    mRunningTimeRemaining--;

    if (mRunningTimeRemaining == 0)
    {
        EndProgram();
        return;
    }

    uint8_t currentPhase = 0;

    if (mRunningTimeRemaining < 5)
    {
        currentPhase = 4;
    }
    else if (mRunningTimeRemaining < 10)
    {
        currentPhase = 3;
    }
    else if (mRunningTimeRemaining < 15)
    {
        currentPhase = 2;
    }
    else if (mRunningTimeRemaining < 20)
    {
        currentPhase = 1;
    }

    OperationalState::GetInstance()->UpdateCountdownTimeFromDelegate();
    UpdateCurrentPhase(currentPhase);
}

DataModel::Nullable<uint32_t> DishwasherManager::GetCountdownTime() const
{
    if (!mIsProgramSelected)
    {
        return DataModel::NullNullable;
    }
    return DataModel::MakeNullable(mDelayedStartTimeRemaining + mRunningTimeRemaining);
}

//****************************
//* MATTER ATTRIBUTE UPDATES *
//****************************

void DishwasherManager::UpdateCurrentPhase(uint8_t phase)
{
    mPhase = phase;
    TEMPORARY_RETURN_IGNORED OperationalState::GetInstance()->SetCurrentPhase(DataModel::MakeNullable(phase));
    UpdateDishwasherDisplay();
}

void DishwasherManager::UpdateOperationState(OperationalStateEnum state)
{
    mState = state;
    TEMPORARY_RETURN_IGNORED OperationalState::GetInstance()->SetOperationalState(to_underlying(state));
    OperationalState::GetInstance()->UpdateCountdownTimeFromDelegate();
    UpdateDishwasherDisplay();
}

void DishwasherManager::UpdateMode(uint8_t mode)
{
    mMode               = mode;
    mIsSettingsSelected = false;

    // The chooser's delays were worked out for the old program.
    if (mScreen == Screen::kStartChoice)
    {
        mScreen = Screen::kStatus;
    }
    UpdateDishwasherDisplay();
}

void DishwasherManager::SetForecast()
{
    // Get the current time the CHIP way... via the Time Synchronization cluster, as there's no SNTP over Thread.
    uint32_t now = 0;
    if (!WallClock::Instance().GetMatterEpochSeconds(now))
    {
        ChipLogError(AppServer, "Time not synchronised; not publishing a forecast. "
                                "Use `chip-tool timesynchronization set-utctime` to set it.");
        return;
    }

    sForecastStruct.forecastID           = ++mCurrentForecastId;
    sForecastStruct.forecastUpdateReason = DeviceEnergyManagement::ForecastUpdateReasonEnum::kInternalOptimization;
    sForecastStruct.startTime            = now + mDelayedStartTimeRemaining;
    sForecastStruct.endTime              = now + mDelayedStartTimeRemaining + mRunningTimeRemaining;

    if (mCanMoveStart)
    {
        sForecastStruct.earliestStartTime.SetValue(DataModel::MakeNullable(now));
        sForecastStruct.latestEndTime.SetValue(now + 86400 /* 24 hours */);
    }
    else
    {
        sForecastStruct.earliestStartTime.ClearValue();
        sForecastStruct.latestEndTime.ClearValue();
    }

    sForecastStruct.isPausable = false;         // We cannot pause any of the slots in this forecast.
    sForecastStruct.activeSlotNumber.SetNull(); // TODO Change this accordingly as the program progresses.

    // The number of slots mirrors the selected mode (Eco is one slot, Chef is 2 and Quick is three).
    size_t slotCount = mMode + 1u;
    for (size_t i = 0; i < slotCount; i++)
    {
        sSlots[i].minDuration     = kSlotDurationS;
        sSlots[i].maxDuration     = kSlotDurationS;
        sSlots[i].defaultDuration = kSlotDurationS;
        sSlots[i].nominalPower.SetValue(kSlotPowerMw);
        sSlots[i].minPower.SetValue(kSlotPowerMw);
        sSlots[i].maxPower.SetValue(kSlotPowerMw);
    }

    sForecastStruct.slots = DataModel::List<const DeviceEnergyManagement::Structs::SlotStruct::Type>(sSlots, slotCount);

    TEMPORARY_RETURN_IGNORED DeviceEnergyManagement::GetDEMDelegate().SetForecast(DataModel::MakeNullable(sForecastStruct));
}

void DishwasherManager::ClearForecast()
{
    TEMPORARY_RETURN_IGNORED DeviceEnergyManagement::GetDEMDelegate().SetForecast(DataModel::NullNullable);
}

//***********
//* DISPLAY *
//***********

void DishwasherManager::UpdateDishwasherDisplay()
{
    const char * stateText = "";

    switch (mState)
    {
    case OperationalStateEnum::kRunning:
        stateText = "RUNNING";
        break;
    case OperationalStateEnum::kPaused:
        stateText = "PAUSED";
        break;
    case OperationalStateEnum::kStopped:
        stateText = "STOPPED";
        break;
    case OperationalStateEnum::kError:
        stateText = "ERROR";
        break;
    default:
        break;
    }

    char modeBuffer[64] = "";
    if (DishwasherMode::GetDelegate() != nullptr)
    {
        MutableCharSpan label(modeBuffer, sizeof(modeBuffer) - 1);
        if (DishwasherMode::GetDelegate()->GetModeLabelByIndex(mMode, label) == CHIP_NO_ERROR)
        {
            modeBuffer[label.size()] = '\0';
        }
    }

    char phaseBuffer[32] = "";
    if ((mState == OperationalStateEnum::kRunning || mState == OperationalStateEnum::kPaused) &&
        OperationalState::GetDelegate() != nullptr)
    {
        MutableCharSpan label(phaseBuffer, sizeof(phaseBuffer) - 1);
        if (OperationalState::GetDelegate()->GetOperationalPhaseAtIndex(mPhase, label) == CHIP_NO_ERROR)
        {
            phaseBuffer[label.size()] = '\0';
        }
    }

    char priceBuffer[40];
    PriceSubscriber::Instance().FormatCurrentPrice(priceBuffer, sizeof(priceBuffer));

    StatusDisplay::View view;
    view.screen              = mScreen;
    view.hasOptedIn          = mOptedIntoEnergyManagement;
    view.isProgramSelected   = mIsProgramSelected;
    view.isSettingsSelected  = mIsSettingsSelected;
    view.startsIn            = mDelayedStartTimeRemaining;
    view.timeRemaining       = mRunningTimeRemaining;
    view.stateText           = stateText;
    view.modeText            = modeBuffer;
    view.phaseText           = phaseBuffer;
    view.priceText           = priceBuffer;
    view.startChoices        = mStartChoices;
    view.startChoiceCount    = mStartChoiceCount;
    view.selectedStartChoice = mSelectedStartChoice;

    StatusDisplay::Instance().UpdateDisplay(view);
}
