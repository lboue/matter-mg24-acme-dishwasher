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
#include <app/server/Server.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <algorithm>
#include <stdio.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::OperationalState;

DishwasherManager DishwasherManager::sDishwasher;

namespace {
constexpr System::Clock::Seconds32 kTickInterval(1);

// Start in one minute to allow for optimisation when opted into energy management.
constexpr uint32_t kOptimisationDelayS = 60;

// How long Optimizing shows after choosing Smart while opted in, for an energy manager to move the start.
constexpr uint8_t kOptimizingS = 5;

// Indices into OperationalStateDelegate's phase list.
enum Phase : uint8_t
{
    kPreSoak,
    kMainWash,
    kRinse,
    kFinalRinse,
    kDrying,
};

// One step of a program, at constant power. Each step is one slot in the DEM forecast.
struct ProgramStep
{
    uint16_t minutes;
    uint16_t powerW;
    Phase phase;
};

// Shaped like real programs: the ~2 kW heater only runs while it heats the water for the main wash
// and the final rinse, and the circulation pump draws 0.1-0.2 kW the rest of the time. Lengths and
// energy match published figures for each kind of program.

// 210 min, 0.91 kWh. Long and cool, drying on leftover heat.
constexpr ProgramStep kEcoSteps[] = {
    { 15, 100, kPreSoak }, { 12, 2000, kMainWash }, { 60, 150, kMainWash },
    { 30, 100, kRinse },   { 8, 2000, kFinalRinse }, { 85, 10, kDrying },
};

// 135 min, 1.67 kWh. A 70° wash, a long hot rinse held at temperature, and a heated dry.
constexpr ProgramStep kChefSteps[] = {
    { 10, 100, kPreSoak },     { 18, 2000, kMainWash },   { 40, 200, kMainWash }, { 15, 100, kRinse },
    { 20, 2000, kFinalRinse }, { 10, 500, kFinalRinse }, { 22, 400, kDrying },
};

// 30 min, 0.53 kWh. No pre-wash, and only a short drain at the end.
constexpr ProgramStep kQuickSteps[] = {
    { 9, 2000, kMainWash },
    { 12, 150, kMainWash },
    { 6, 2000, kFinalRinse },
    { 3, 50, kDrying },
};

// 120 min, 1.34 kWh. A heated pre-wash, a 65° wash and a hot final rinse.
constexpr ProgramStep kHeavySteps[] = {
    { 8, 2000, kPreSoak }, { 15, 2000, kMainWash },   { 35, 200, kMainWash },
    { 15, 100, kRinse },   { 12, 2000, kFinalRinse }, { 35, 50, kDrying },
};

struct Program
{
    const ProgramStep * steps;
    size_t count;
};

template <size_t N>
constexpr Program MakeProgram(const ProgramStep (&steps)[N])
{
    return { steps, N };
}

// Indexed by mode.
constexpr Program kPrograms[DishwasherMode::DishwasherModeDelegate::kModeCount] = {
    MakeProgram(kEcoSteps),
    MakeProgram(kChefSteps),
    MakeProgram(kQuickSteps),
    MakeProgram(kHeavySteps),
};

constexpr size_t kMaxProgramSteps = 7;
static_assert(MATTER_ARRAY_SIZE(kEcoSteps) <= kMaxProgramSteps && MATTER_ARRAY_SIZE(kChefSteps) <= kMaxProgramSteps &&
              MATTER_ARRAY_SIZE(kQuickSteps) <= kMaxProgramSteps && MATTER_ARRAY_SIZE(kHeavySteps) <= kMaxProgramSteps);

// How far ahead a program may be moved, matching the forecast's latestEndTime.
constexpr uint32_t kStartHorizonS = 86400;

// Turning the knob on the Ready By screen moves it by this much.
constexpr uint32_t kReadyByStepS = 15 * 60;

uint32_t RoundUpToStep(uint32_t seconds)
{
    return (seconds + kReadyByStepS - 1) / kReadyByStepS * kReadyByStepS;
}

using Screen = StatusDisplay::Screen;

// The current period plus the forecast, for StartPlanner. Only used on the Matter thread.
StartPlanner::Period sPlannerPeriods[1 + PriceSubscriber::kMaxForecastEntries];
static_assert(PriceSubscriber::kMaxForecastEntries >= GridConditionsSubscriber::kMaxForecastEntries);

// The selected program's steps, for StartPlanner.
StartPlanner::Step sPowerProfile[kMaxProgramSteps];

// Track this separately as the forecast struct only holds a list view onto the slots.
DeviceEnergyManagement::Structs::SlotStruct::Type sSlots[kMaxProgramSteps];
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
    DishwasherMgr().CheckCommissioningWindow();
    DishwasherMgr().TickOptimizing();
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

    if (mScreen == Screen::kSettings || mScreen == Screen::kEnergyMenu)
    {
        const int count = mScreen == Screen::kSettings ? int{ StatusDisplay::kSettingsItemCount }
                                                       : int{ StatusDisplay::kEnergyItemCount };
        // Wraps around at either end.
        mMenuItem = static_cast<uint8_t>((mMenuItem + count + direction) % count);
        UpdateDishwasherDisplay();
        return;
    }

    if (mScreen == Screen::kCommissioning || mScreen == Screen::kOptimizing)
    {
        return;
    }

    if (mScreen == Screen::kSmartStart)
    {
        // The only option is Cancel, so turning either way highlights or unhighlights it.
        mMenuItem = mMenuItem == 0 ? 1 : 0;
        UpdateDishwasherDisplay();
        return;
    }

    if (mScreen == Screen::kReadyBy)
    {
        // From the earliest it could be ready to a day ahead, without wrapping. With the clock, each
        // step stays on a quarter hour.
        RefreshReadyBy();
        int32_t step = 0;
        if (direction > 0 && mReadyByS + kReadyByStepS <= kStartHorizonS)
        {
            step = kReadyByStepS;
        }
        else if (direction < 0 && mReadyByS >= EarliestReadyByS() + kReadyByStepS)
        {
            step = -static_cast<int32_t>(kReadyByStepS);
        }
        else
        {
            return;
        }
        mReadyByS += step;
        if (mReadyByAt != 0)
        {
            mReadyByAt += step;
        }
        UpdateReadyBy();
        UpdateDishwasherDisplay();
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

    switch (mScreen)
    {
    case Screen::kSettings:
        switch (mMenuItem)
        {
        case StatusDisplay::kSettingsCommissioning:
            OpenCommissioning();
            break;
        case StatusDisplay::kSettingsEnergy:
            ShowScreen(Screen::kEnergyMenu, mOptedIntoEnergyManagement ? StatusDisplay::kEnergyOptIn : StatusDisplay::kEnergyOptOut);
            break;
        default:
            ShowScreen(Screen::kStatus);
            break;
        }
        return;
    case Screen::kCommissioning:
        CloseCommissioning();
        ShowScreen(Screen::kSettings, StatusDisplay::kSettingsCommissioning);
        return;
    case Screen::kEnergyMenu:
        SetEnergyManagementOptIn(mMenuItem == StatusDisplay::kEnergyOptIn);
        ShowScreen(Screen::kSettings, StatusDisplay::kSettingsEnergy);
        return;
    case Screen::kOptimizing:
        return;
    case Screen::kSmartStart:
        if (mMenuItem == 1)
        {
            CancelSmartStart();
        }
        else
        {
            ShowScreen(Screen::kStatus);
        }
        return;
    default:
        break;
    }

    if (mScreen == Screen::kReadyBy)
    {
        StartReadyBy();
        return;
    }

    if (mIsProgramSelected)
    {
        if (mDelayedStartTimeRemaining > 0)
        {
            ChipLogProgress(AppServer, "Waiting to start; press BTN0 to cancel");
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
        ShowScreen(Screen::kSettings);
        return;
    }

    OpenReadyBy();
}

// BTN0: cancel, or back one level.
void DishwasherManager::HandleCancelButton()
{
    if (!mIsPoweredOn)
    {
        return;
    }

    switch (mScreen)
    {
    case Screen::kCommissioning:
        CloseCommissioning();
        ShowScreen(Screen::kSettings, StatusDisplay::kSettingsCommissioning);
        return;
    case Screen::kEnergyMenu:
        ShowScreen(Screen::kSettings, StatusDisplay::kSettingsEnergy);
        return;
    case Screen::kSettings:
    case Screen::kReadyBy:
        ShowScreen(Screen::kStatus);
        return;
    case Screen::kOptimizing:
    case Screen::kSmartStart:
        CancelSmartStart();
        return;
    case Screen::kStatus:
        break;
    }

    if (mIsProgramSelected)
    {
        StopProgram();
    }
}

void DishwasherManager::ShowScreen(Screen screen, uint8_t menuItem)
{
    mScreen   = screen;
    mMenuItem = menuItem;
    UpdateDishwasherDisplay();
}

void DishwasherManager::OpenCommissioning()
{
    auto & windowManager = Server::GetInstance().GetCommissioningWindowManager();
    if (!windowManager.IsCommissioningWindowOpen())
    {
        CHIP_ERROR err = windowManager.OpenBasicCommissioningWindow();
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(AppServer, "Failed to open the commissioning window: %" CHIP_ERROR_FORMAT, err.Format());
            return;
        }
    }

    const RendezvousInformationFlags rendezvous(RendezvousInformationFlag::kBLE, RendezvousInformationFlag::kOnNetwork);

    MutableCharSpan qrCode(mQrCode, sizeof(mQrCode) - 1);
    if (GetQRCode(qrCode, rendezvous) == CHIP_NO_ERROR)
    {
        mQrCode[qrCode.size()] = '\0';
    }
    else
    {
        mQrCode[0] = '\0';
    }

    MutableCharSpan manualCode(mManualCode, sizeof(mManualCode) - 1);
    if (GetManualPairingCode(manualCode, rendezvous) == CHIP_NO_ERROR)
    {
        mManualCode[manualCode.size()] = '\0';
    }
    else
    {
        mManualCode[0] = '\0';
    }

    ChipLogProgress(AppServer, "Commissioning window open. QR code %s, manual code %s", mQrCode, mManualCode);
    ShowScreen(Screen::kCommissioning);
}

void DishwasherManager::CloseCommissioning()
{
    auto & windowManager = Server::GetInstance().GetCommissioningWindowManager();
    if (windowManager.IsCommissioningWindowOpen())
    {
        windowManager.CloseCommissioningWindow();
        ChipLogProgress(AppServer, "Commissioning window closed");
    }
}

void DishwasherManager::CheckCommissioningWindow()
{
    // The window closes by itself when it times out or a controller finishes commissioning.
    if (mScreen == Screen::kCommissioning && !Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen())
    {
        ChipLogProgress(AppServer, "Commissioning window has closed");
        ShowScreen(Screen::kSettings, StatusDisplay::kSettingsCommissioning);
    }
}

void DishwasherManager::OpenReadyBy()
{
    // Start on the earliest Ready By that gets the biggest saving: the best start within a day,
    // plus the run time. Without a forecast, the earliest it could be ready.
    mReadyByS  = kStartHorizonS;
    mReadyByAt = 0;
    UpdateReadyBy();
    SetReadyByFromNow(mReadyByHasStart ? mReadyByDelayS + GetProgramDuration() : GetProgramDuration());

    // Rounding up to a quarter hour can take it just past a day ahead.
    while (mReadyByS > kStartHorizonS && mReadyByS >= EarliestReadyByS() + kReadyByStepS)
    {
        mReadyByS -= kReadyByStepS;
        mReadyByAt -= mReadyByAt != 0 ? kReadyByStepS : 0;
    }

    UpdateReadyBy();
    ShowScreen(Screen::kReadyBy);
}

// Sets Ready By to at least secondsFromNow, rounded up to the next quarter hour on the clock, or
// to the next 15 minutes from now without the clock.
void DishwasherManager::SetReadyByFromNow(uint32_t secondsFromNow)
{
    uint32_t now = 0;
    if (WallClock::Instance().GetMatterEpochSeconds(now))
    {
        mReadyByAt = RoundUpToStep(now + secondsFromNow);
        mReadyByS  = mReadyByAt - now;
    }
    else
    {
        mReadyByAt = 0;
        mReadyByS  = RoundUpToStep(secondsFromNow);
    }
}

// With the clock, Ready By is a fixed clock time, so the time left until then shrinks as time passes.
void DishwasherManager::RefreshReadyBy()
{
    uint32_t now = 0;
    if (mReadyByAt != 0 && WallClock::Instance().GetMatterEpochSeconds(now))
    {
        mReadyByS = mReadyByAt > now ? mReadyByAt - now : 0;
    }
}

// The earliest Ready By, in seconds from now: the end of a run started now, rounded up as above.
uint32_t DishwasherManager::EarliestReadyByS() const
{
    uint32_t now = 0;
    if (mReadyByAt != 0 && WallClock::Instance().GetMatterEpochSeconds(now))
    {
        return RoundUpToStep(now + GetProgramDuration()) - now;
    }
    return RoundUpToStep(GetProgramDuration());
}

// Finds the best start that finishes by Ready By (price first, then grid conditions), and when
// opted out, what it saves compared with starting now.
void DishwasherManager::UpdateReadyBy()
{
    RefreshReadyBy();

    mReadyByHasStart = false;
    mSavingText[0]   = '\0';

    Span<const StartPlanner::Period> periods;
    uint32_t now       = 0;
    uint32_t delayS    = 0;
    const auto profile = GetPowerProfile();
    if (BuildPricePeriods(periods, now) && StartPlanner::FindBestStart(periods, profile, now, mReadyByS, delayS))
    {
        mSmartUsesPrice = true;
    }
    else if (BuildGridPeriods(periods, now) && StartPlanner::FindBestStart(periods, profile, now, mReadyByS, delayS))
    {
        mSmartUsesPrice = false;
    }
    else
    {
        return;
    }

    mReadyByHasStart = true;
    mReadyByDelayS   = delayS;

    // When opted in, an energy manager may move the start, so there's no saving to promise.
    if (!mOptedIntoEnergyManagement)
    {
        FormatSaving(periods, now, delayS);
    }
}

void DishwasherManager::StartReadyBy()
{
    if (!mReadyByHasStart)
    {
        // No forecast or no clock, so nothing to optimise.
        ChipLogProgress(AppServer, "Nothing to optimise against, starting now");
        BeginProgram(0, false);
        return;
    }

    const uint32_t delayS = mReadyByDelayS;

    RefreshReadyBy();
    uint32_t now  = 0;
    mReadyByEpoch = mReadyByAt != 0 ? mReadyByAt : (WallClock::Instance().GetMatterEpochSeconds(now) ? now + mReadyByS : 0);

    ChipLogProgress(AppServer, "Ready by %lus from now: starting in %lus (%s)", static_cast<unsigned long>(mReadyByS),
                    static_cast<unsigned long>(delayS), mSmartUsesPrice ? "price" : "grid");
    BeginProgram(delayS, mOptedIntoEnergyManagement);

    if (mOptedIntoEnergyManagement)
    {
        // Schedule it at the cheapest time, but hold it while Optimizing shows, even if that time is
        // now, so an energy manager can still move it.
        mHoldingStart        = true;
        mOptimizingTicksLeft = kOptimizingS;
        ShowScreen(Screen::kOptimizing);
    }
    else if (delayS > 0)
    {
        ShowSmartStart();
    }
}

uint32_t DishwasherManager::GetProgramDuration() const
{
    uint32_t durationS = 0;
    for (size_t i = 0; i < kPrograms[mMode].count; i++)
    {
        durationS += kPrograms[mMode].steps[i].minutes * 60u;
    }
    return durationS;
}

Span<const StartPlanner::Step> DishwasherManager::GetPowerProfile() const
{
    const Program & program = kPrograms[mMode];
    for (size_t i = 0; i < program.count; i++)
    {
        sPowerProfile[i] = { program.steps[i].minutes * 60u, program.steps[i].powerW };
    }
    return Span<const StartPlanner::Step>(sPowerProfile, program.count);
}

// Fills sPlannerPeriods with the current price and the price forecast. False without a forecast or the time.
bool DishwasherManager::BuildPricePeriods(Span<const StartPlanner::Period> & periods, uint32_t & now) const
{
    const auto & prices = PriceSubscriber::Instance();
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

    periods = Span<const StartPlanner::Period>(sPlannerPeriods, count);
    return true;
}

// The same for the grid's carbon intensity.
bool DishwasherManager::BuildGridPeriods(Span<const StartPlanner::Period> & periods, uint32_t & now) const
{
    const auto & grid = GridConditionsSubscriber::Instance();
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

    periods = Span<const StartPlanner::Period>(sPlannerPeriods, count);
    return true;
}

// While Optimizing shows, an energy manager can move the start. Then show where it ended up.
void DishwasherManager::TickOptimizing()
{
    if (mScreen != Screen::kOptimizing)
    {
        return;
    }

    if (mOptimizingTicksLeft > 1)
    {
        mOptimizingTicksLeft--;
        UpdateDishwasherDisplay();
        return;
    }

    mHoldingStart = false;

    // Nothing to wait for, so it starts on the next tick.
    if (mDelayedStartTimeRemaining == 0)
    {
        ShowScreen(Screen::kStatus);
        return;
    }

    ShowSmartStart();
}

void DishwasherManager::ShowSmartStart()
{
    UpdateSmartSaving();
    ShowScreen(Screen::kSmartStart, 0);
}

// Drops the scheduled Smart start and goes back to the home screen, with the same program selected.
void DishwasherManager::CancelSmartStart()
{
    ChipLogProgress(AppServer, "Smart start cancelled");

    // StopProgram() goes back to Eco, so put the program back afterwards.
    const uint8_t mode = mMode;
    StopProgram();
    SelectMode(mode);
    ShowScreen(Screen::kStatus);
}

// What starting at the scheduled time saves compared with starting now, for the Smart Start screen.
void DishwasherManager::UpdateSmartSaving()
{
    mSavingText[0] = '\0';

    Span<const StartPlanner::Period> periods;
    uint32_t now = 0;
    if (!(mSmartUsesPrice ? BuildPricePeriods(periods, now) : BuildGridPeriods(periods, now)))
    {
        return;
    }

    FormatSaving(periods, now, mDelayedStartTimeRemaining);
}

// Sets mSavingText to what starting delayS from now saves compared with starting now.
void DishwasherManager::FormatSaving(Span<const StartPlanner::Period> periods, uint32_t now, uint32_t delayS)
{
    mSavingText[0] = '\0';

    const auto profile = GetPowerProfile();
    int64_t costNow    = 0;
    int64_t costLater  = 0;
    if (!StartPlanner::ProfileCost(periods, profile, now, costNow) ||
        !StartPlanner::ProfileCost(periods, profile, now + delayS, costLater))
    {
        return;
    }

    // The costs are value × W × s, so this gives value × kWh: money, or grams of CO2. Rounded.
    constexpr int64_t kWattSecondsPerKWh = 3'600'000;
    const int64_t saving                 = costNow - costLater;
    const int64_t magnitude              = ((saving < 0 ? -saving : saving) + kWattSecondsPerKWh / 2) / kWattSecondsPerKWh;

    if (magnitude == 0)
    {
        snprintf(mSavingText, sizeof(mSavingText), "No saving");
        return;
    }

    char amount[24];
    if (mSmartUsesPrice)
    {
        PriceSubscriber::Instance().FormatAmount(magnitude, amount, sizeof(amount));
    }
    else
    {
        snprintf(amount, sizeof(amount), "%ld gCO2", static_cast<long>(magnitude));
    }

    if (saving > 0)
    {
        snprintf(mSavingText, sizeof(mSavingText), "Saves %s", amount);
    }
    else
    {
        snprintf(mSavingText, sizeof(mSavingText), "Costs %s more", amount);
    }
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
    if (mScreen == Screen::kCommissioning)
    {
        CloseCommissioning();
    }

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

void DishwasherManager::BeginProgram(uint32_t delayS, bool canMoveStart)
{
    if (mIsProgramSelected)
    {
        ChipLogProgress(AppServer, "A program has already been started");
        return;
    }

    if (mScreen == Screen::kCommissioning)
    {
        CloseCommissioning();
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
    VerifyOrReturnError(mIsProgramSelected && mState == OperationalStateEnum::kStopped &&
                            (mDelayedStartTimeRemaining > 0 || mHoldingStart),
                        CHIP_ERROR_INCORRECT_STATE, ChipLogError(AppServer, "Program already started, cannot adjust start time"));

    uint32_t now = 0;
    VerifyOrReturnError(WallClock::Instance().GetMatterEpochSeconds(now), CHIP_ERROR_INCORRECT_STATE,
                        ChipLogError(AppServer, "Time not synchronised, cannot adjust start time"));

    // A program started from Ready By has to finish by then.
    VerifyOrReturnError(mReadyByEpoch == 0 || newStartTime + mRunningTimeRemaining <= mReadyByEpoch, CHIP_ERROR_INVALID_ARGUMENT,
                        ChipLogError(AppServer, "New start time would finish after Ready By, ignoring"));

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
    if (mScreen == Screen::kOptimizing || mScreen == Screen::kSmartStart)
    {
        mScreen = Screen::kStatus;
    }

    mIsProgramSelected         = false;
    mCanMoveStart              = false;
    mHoldingStart              = false;
    mReadyByEpoch              = 0;
    mDelayedStartTimeRemaining = 0;
    mRunningTimeRemaining      = 0;
    UpdateCurrentPhase(0);

    // Back to the default program, and keep the DishwasherMode cluster in step.
    if (mMode != DishwasherMode::ModeEco && DishwasherMode::GetInstance() != nullptr)
    {
        TEMPORARY_RETURN_IGNORED DishwasherMode::GetInstance()->UpdateCurrentMode(DishwasherMode::ModeEco);
    }
    UpdateMode(DishwasherMode::ModeEco);

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

    // Held while an energy manager may still move the start.
    if (mState == OperationalStateEnum::kStopped && mHoldingStart)
    {
        return;
    }

    // If we are stopped, we should start running.
    if (mState == OperationalStateEnum::kStopped)
    {
        if (mScreen == Screen::kOptimizing || mScreen == Screen::kSmartStart)
        {
            mScreen = Screen::kStatus;
        }
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

    // The phase follows the program's steps.
    const Program & program = kPrograms[mMode];
    uint32_t elapsedS       = GetProgramDuration() - mRunningTimeRemaining;
    uint8_t currentPhase    = program.steps[program.count - 1].phase;
    for (size_t i = 0; i < program.count; i++)
    {
        const uint32_t stepS = program.steps[i].minutes * 60u;
        if (elapsedS < stepS)
        {
            currentPhase = program.steps[i].phase;
            break;
        }
        elapsedS -= stepS;
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

    // Ready By was worked out for the old program.
    if (mScreen == Screen::kReadyBy)
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
        // The Ready By deadline, if there is one, otherwise a day.
        sForecastStruct.latestEndTime.SetValue(mReadyByEpoch != 0 ? mReadyByEpoch : now + kStartHorizonS);
    }
    else
    {
        sForecastStruct.earliestStartTime.ClearValue();
        sForecastStruct.latestEndTime.ClearValue();
    }

    sForecastStruct.isPausable = false;         // We cannot pause any of the slots in this forecast.
    sForecastStruct.activeSlotNumber.SetNull(); // TODO Change this accordingly as the program progresses.

    // One slot per program step.
    const Program & program = kPrograms[mMode];
    size_t slotCount        = program.count;
    for (size_t i = 0; i < slotCount; i++)
    {
        const uint32_t durationS = program.steps[i].minutes * 60u;
        const int64_t powerMw    = program.steps[i].powerW * 1000;

        sSlots[i].minDuration     = durationS;
        sSlots[i].maxDuration     = durationS;
        sSlots[i].defaultDuration = durationS;
        sSlots[i].nominalPower.SetValue(powerMw);
        sSlots[i].minPower.SetValue(powerMw);
        sSlots[i].maxPower.SetValue(powerMw);
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
    view.programDurationS    = GetProgramDuration();
    view.stateText           = stateText;
    view.modeText            = modeBuffer;
    view.phaseText           = phaseBuffer;
    view.priceText           = priceBuffer;
    if (mScreen == Screen::kReadyBy)
    {
        RefreshReadyBy();
    }
    view.readyByS            = mReadyByS;
    view.readyByClock        = mReadyByAt;
    if (mScreen == Screen::kSmartStart)
    {
        // When it will finish: the start plus the run time.
        uint32_t now      = 0;
        view.readyByClock = WallClock::Instance().GetMatterEpochSeconds(now) ? now + mDelayedStartTimeRemaining + mRunningTimeRemaining
                                                                             : 0;
    }
    view.hasClock            = view.readyByClock != 0;
    view.selectedMenuItem    = mMenuItem;
    view.qrCode              = mQrCode;
    view.manualCode          = mManualCode;
    view.optimizingDots      = static_cast<uint8_t>((kOptimizingS - mOptimizingTicksLeft) % 3 + 1);
    view.savingText          = mSavingText;

    StatusDisplay::Instance().UpdateDisplay(view);
}
