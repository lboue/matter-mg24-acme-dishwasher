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
#include <string.h>

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

// Whether a price period is over. Without the time, nothing is.
bool HasEnded(const PriceSubscriber::PriceInfo & info, bool hasNow, uint32_t now)
{
    return hasNow && info.periodEnd != 0 && info.periodEnd <= now;
}

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

// Now may start this much later, if that's cheaper (or an energy manager moves it).
constexpr uint32_t kNowWindowS = 30 * 60;

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
    DishwasherMgr().TickHold();
    DishwasherMgr().ProgressProgram();
    DishwasherMgr().RefreshClockScreens();
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

    if (mScreen == Screen::kPricing)
    {
        // Stops at either end, as the list can be long. BuildPriceList() keeps it within the list.
        if (direction < 0 && mPriceCursor > 0)
        {
            mPriceCursor--;
        }
        else if (direction > 0 && mPriceCursor + 1 < mPriceListCount)
        {
            mPriceCursor++;
        }
        UpdateDishwasherDisplay();
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

    if (mScreen == Screen::kCommissioning || mScreen == Screen::kOptimizing || mScreen == Screen::kSmartStart)
    {
        return;
    }

    if (mScreen == Screen::kStartChoice)
    {
        // Two boxes, so turning either way swaps between them.
        mSelectedStartChoice = mSelectedStartChoice == 0 ? 1 : 0;
        UpdateDishwasherDisplay();
        return;
    }

    if (mScreen == Screen::kReadyBy)
    {
        // Through the significant times, in order, without wrapping.
        const int next = mReadyByIndex + direction;
        if (next < 0 || next >= mReadyByTimeCount)
        {
            return;
        }
        SelectReadyByTime(static_cast<uint8_t>(next));
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
        case StatusDisplay::kSettingsPricing:
            mPriceCursor = 0;
            mPriceTop    = 0;
            ShowScreen(Screen::kPricing);
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
    case Screen::kPricing:
        ShowScreen(Screen::kSettings, StatusDisplay::kSettingsPricing);
        return;
    case Screen::kOptimizing:
        return;
    case Screen::kSmartStart:
        // Confirm. BTN0 cancels.
        ShowScreen(Screen::kStatus);
        return;
    default:
        break;
    }

    if (mScreen == Screen::kStartChoice)
    {
        if (mSelectedStartChoice == 0)
        {
            StartNow();
        }
        else
        {
            OpenReadyBy();
        }
        return;
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

    OpenStartChoice();
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
    case Screen::kPricing:
        ShowScreen(Screen::kSettings, StatusDisplay::kSettingsPricing);
        return;
    case Screen::kSettings:
    case Screen::kStartChoice:
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

// Now or Smart, when there's something to plan with. Otherwise just start.
void DishwasherManager::OpenStartChoice()
{
    Span<const StartPlanner::Period> periods;
    uint32_t now = 0;
    if (!BuildPricePeriods(periods, now) && !BuildGridPeriods(periods, now))
    {
        ChipLogProgress(AppServer, "No forecast or no clock, starting now");
        BeginProgram(0, false);
        return;
    }

    mSelectedStartChoice = 0;
    ShowScreen(Screen::kStartChoice);
}

// Starts within the next half hour, at the cheapest time in it. When opted in, an energy manager
// can move it within that half hour.
void DishwasherManager::StartNow()
{
    const uint32_t horizonS = kNowWindowS + GetProgramDuration();

    Span<const StartPlanner::Period> periods;
    uint32_t now    = 0;
    uint32_t delayS = 0;
    if (!FindBestStartWithin(horizonS, delayS, periods, now))
    {
        BeginProgram(0, false);
        return;
    }

    // What waiting saves, before BeginProgram() clears it.
    char delaySaving[sizeof(mDelaySavingText)] = "";
    if (delayS > 0)
    {
        FormatSaving(periods, now, delayS, delaySaving, sizeof(delaySaving), "Delay saves");
    }

    mReadyByEpoch = now + horizonS;
    ChipLogProgress(AppServer, "Starting now: in %lus (%s) %s", static_cast<unsigned long>(delayS),
                    mSmartUsesPrice ? "price" : "grid", delaySaving);
    BeginProgram(delayS, mOptedIntoEnergyManagement);
    strncpy(mDelaySavingText, delaySaving, sizeof(mDelaySavingText));

    // Even when the best time is now, hold it a moment so an energy manager can still move it.
    if (mOptedIntoEnergyManagement && delayS == 0)
    {
        mHoldingStart  = true;
        mHoldTicksLeft = kOptimizingS;
    }
}

// Smart: choose when it must be ready, from the significant times of day.
void DishwasherManager::OpenReadyBy()
{
    uint32_t now = 0;
    VerifyOrReturn(WallClock::Instance().GetMatterEpochSeconds(now), BeginProgram(0, false));

    const uint32_t durationS = GetProgramDuration();
    mReadyByTimeCount        = static_cast<uint8_t>(
        StartPlanner::SignificantTimes(now + durationS, now + kStartHorizonS, mReadyByTimes, kMaxReadyByTimes));
    VerifyOrReturn(mReadyByTimeCount > 0, BeginProgram(0, false));

    // Start on the earliest of them that gets the biggest saving: after the best start within a day
    // has finished.
    mReadyByAt = 0;
    mReadyByS  = kStartHorizonS;
    UpdateReadyBy();
    const uint32_t bestFinish = now + (mReadyByHasStart ? mReadyByDelayS : 0) + durationS;

    uint8_t index = static_cast<uint8_t>(mReadyByTimeCount - 1);
    for (uint8_t i = 0; i < mReadyByTimeCount; i++)
    {
        if (mReadyByTimes[i] >= bestFinish)
        {
            index = i;
            break;
        }
    }

    SelectReadyByTime(index);
    ShowScreen(Screen::kReadyBy);
}

void DishwasherManager::SelectReadyByTime(uint8_t index)
{
    mReadyByIndex = index;
    mReadyByAt    = mReadyByTimes[index];
    UpdateReadyBy();
}

// Ready By is a fixed clock time, so the time left until then shrinks as time passes.
void DishwasherManager::RefreshReadyBy()
{
    uint32_t now = 0;
    if (mReadyByAt != 0 && WallClock::Instance().GetMatterEpochSeconds(now))
    {
        mReadyByS = mReadyByAt > now ? mReadyByAt - now : 0;
    }
}

// The best start that finishes within horizonS: by price, or with no price forecast, by grid
// conditions. Also gives the periods and time it planned with.
bool DishwasherManager::FindBestStartWithin(uint32_t horizonS, uint32_t & delayS, Span<const StartPlanner::Period> & periods,
                                            uint32_t & now)
{
    const auto profile = GetPowerProfile();
    if (BuildPricePeriods(periods, now) && StartPlanner::FindBestStart(periods, profile, now, horizonS, delayS))
    {
        mSmartUsesPrice = true;
        return true;
    }
    if (BuildGridPeriods(periods, now) && StartPlanner::FindBestStart(periods, profile, now, horizonS, delayS))
    {
        mSmartUsesPrice = false;
        return true;
    }
    return false;
}

// Finds the best start that finishes by Ready By (price first, then grid conditions), and when
// opted out, what it saves compared with starting now.
void DishwasherManager::UpdateReadyBy()
{
    RefreshReadyBy();

    mReadyByHasStart = false;
    mSavingText[0]   = '\0';
    mCheapSpanCount  = 0;

    Span<const StartPlanner::Period> periods;
    uint32_t now    = 0;
    uint32_t delayS = 0;
    if (!FindBestStartWithin(mReadyByS, delayS, periods, now))
    {
        return;
    }

    mReadyByHasStart = true;
    mReadyByDelayS   = delayS;

    UpdateCheapSpans(periods, now);

    // When opted in, an energy manager may move the start, so there's no saving to promise.
    if (!mOptedIntoEnergyManagement)
    {
        FormatSaving(periods, now, delayS, mSavingText, sizeof(mSavingText));
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
        mHoldingStart  = true;
        mHoldTicksLeft = kOptimizingS;
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

// Counts down a held start, during which an energy manager can move it. After Optimizing (Smart),
// shows where the start ended up.
void DishwasherManager::TickHold()
{
    if (!mHoldingStart)
    {
        return;
    }

    if (mHoldTicksLeft > 1)
    {
        mHoldTicksLeft--;
        if (mScreen == Screen::kOptimizing)
        {
            UpdateDishwasherDisplay();
        }
        return;
    }

    mHoldingStart = false;

    // Smart shows where the start ended up. Now just carries on.
    if (mScreen != Screen::kOptimizing)
    {
        return;
    }

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

    FormatSaving(periods, now, mDelayedStartTimeRemaining, mSavingText, sizeof(mSavingText));
}

// The periods in the next day in the cheapest quarter (by price, or carbon intensity), as merged
// spans of seconds from now, for shading on the Ready By timeline.
void DishwasherManager::UpdateCheapSpans(Span<const StartPlanner::Period> periods, uint32_t now)
{
    mCheapSpanCount = 0;

    const uint32_t end = now + kStartHorizonS;
    auto inNextDay     = [now, end](const StartPlanner::Period & period) {
        return period.start < end && (period.end == 0 || period.end > now);
    };

    int64_t values[1 + PriceSubscriber::kMaxForecastEntries];
    size_t count = 0;
    for (const auto & period : periods)
    {
        if (inNextDay(period) && count < MATTER_ARRAY_SIZE(values))
        {
            values[count++] = period.value;
        }
    }
    if (count < 2)
    {
        return;
    }

    // Nothing stands out if every period is the same.
    const auto [lowest, highest] = std::minmax_element(values, values + count);
    if (*lowest == *highest)
    {
        return;
    }

    std::nth_element(values, values + count / 4, values + count);
    const int64_t threshold = values[count / 4];

    for (const auto & period : periods)
    {
        if (!inNextDay(period) || period.value > threshold)
        {
            continue;
        }

        const uint32_t startS = (period.start > now ? period.start : now) - now;
        const uint32_t endS   = ((period.end == 0 || period.end > end) ? end : period.end) - now;

        // Merge with the last span if they touch or overlap (the current period repeats the first
        // forecast entry).
        if (mCheapSpanCount > 0 && startS <= mCheapSpans[mCheapSpanCount - 1].endS &&
            endS >= mCheapSpans[mCheapSpanCount - 1].startS)
        {
            auto & last = mCheapSpans[mCheapSpanCount - 1];
            last.startS = std::min(last.startS, startS);
            last.endS   = std::max(last.endS, endS);
        }
        else if (mCheapSpanCount < StatusDisplay::kMaxCheapSpans)
        {
            mCheapSpans[mCheapSpanCount++] = { startS, endS };
        }
    }
}

// Writes what starting delayS from now saves compared with starting now, e.g. "<savesWord> 0.18 GBP".
void DishwasherManager::FormatSaving(Span<const StartPlanner::Period> periods, uint32_t now, uint32_t delayS, char * buffer,
                                     size_t size, const char * savesWord)
{
    buffer[0] = '\0';

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
        snprintf(buffer, size, "No saving");
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
        snprintf(buffer, size, "%s %s", savesWord, amount);
    }
    else
    {
        snprintf(buffer, size, "Costs %s more", amount);
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
    mDelaySavingText[0] = '\0';
    mCanMoveStart       = canMoveStart;

    mRunningTimeRemaining      = GetProgramDuration();
    mPhase                     = 0;
    mDelayedStartTimeRemaining = delayS;

    SetForecast();
    UpdateDishwasherDisplay();
}

CHIP_ERROR DishwasherManager::AdjustStartTime(uint32_t newStartTime, AdjustmentCauseEnum cause)
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
    mDelaySavingText[0]        = '\0'; // Worked out for the old start
    ChipLogProgress(AppServer, "Program will now start in %lus", static_cast<unsigned long>(mDelayedStartTimeRemaining));

    // Publish the moved forecast ourselves, so it has our next forecast ID and says who moved it.
    SetForecast(cause == AdjustmentCauseEnum::kGridOptimization ? ForecastUpdateReason::kGridOptimization
                                                                : ForecastUpdateReason::kLocalOptimization);

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
    mDelaySavingText[0]        = '\0';
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

    // START and Ready By were worked out for the old program.
    if (mScreen == Screen::kStartChoice || mScreen == Screen::kReadyBy)
    {
        mScreen = Screen::kStatus;
    }
    UpdateDishwasherDisplay();
}

void DishwasherManager::SetForecast(ForecastUpdateReason reason)
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
    sForecastStruct.forecastUpdateReason = reason;
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

void DishwasherManager::BuildPriceList()
{
    constexpr uint8_t kLines = StatusDisplay::kMaxPriceLines;

    PriceSubscriber & prices = PriceSubscriber::Instance();
    uint32_t now             = 0;
    const bool hasNow        = WallClock::Instance().GetMatterEpochSeconds(now);

    PriceSubscriber::PriceInfo current;
    const bool showCurrent = prices.GetCurrentPrice(current) && current.hasPrice && !HasEnded(current, hasNow, now);
    const auto forecast    = prices.GetForecast();

    // The rows in order: the current price, then the forecast periods that haven't ended. Calls
    // visit(row, info) for each, and returns how many there are.
    auto forEachRow = [&](auto && visit) {
        size_t count = 0;
        if (showCurrent)
        {
            visit(count++, current);
        }
        for (const auto & entry : forecast)
        {
            if ((!entry.hasPrice && !entry.hasPriceLevel) || HasEnded(entry, hasNow, now) ||
                (showCurrent && entry.periodStart == current.periodStart))
            {
                continue;
            }
            visit(count++, entry);
        }
        return count;
    };

    mPriceListCount = static_cast<uint8_t>(forEachRow([](size_t, const PriceSubscriber::PriceInfo &) {}));
    if (mPriceListCount == 0)
    {
        mPriceCursor    = 0;
        mPriceTop       = 0;
        mPriceLineCount = 1;
        prices.FormatCurrentPrice(mPriceLines[0].left, sizeof(mPriceLines[0].left));
        mPriceLines[0].right[0] = '\0';
        return;
    }

    // Keep the cursor in the list, which shrinks as periods end, and on screen.
    mPriceCursor = std::min<uint8_t>(mPriceCursor, static_cast<uint8_t>(mPriceListCount - 1));
    if (mPriceCursor < mPriceTop)
    {
        mPriceTop = mPriceCursor;
    }
    else if (mPriceCursor >= mPriceTop + kLines)
    {
        mPriceTop = static_cast<uint8_t>(mPriceCursor - kLines + 1);
    }
    mPriceTop = std::min<uint8_t>(mPriceTop, mPriceListCount > kLines ? static_cast<uint8_t>(mPriceListCount - kLines) : 0);

    mPriceLineCount = 0;
    forEachRow([&](size_t row, const PriceSubscriber::PriceInfo & info) {
        if (row < mPriceTop || row >= mPriceTop + kLines)
        {
            return;
        }
        StatusDisplay::MenuLine & line = mPriceLines[mPriceLineCount++];

        char value[16] = "";
        if (info.hasPrice)
        {
            prices.FormatValue(info.price, value, sizeof(value));
        }

        const bool isNow = hasNow && info.periodStart <= now && (info.periodEnd == 0 || now < info.periodEnd);
        if (isNow)
        {
            snprintf(line.left, sizeof(line.left), "Now   %s", value);
        }
        else
        {
            const uint32_t secondOfDay = info.periodStart % (24 * 60 * 60);
            snprintf(line.left, sizeof(line.left), "%02u:%02u %s", static_cast<unsigned>(secondOfDay / 3600),
                     static_cast<unsigned>((secondOfDay / 60) % 60), value);
        }
        snprintf(line.right, sizeof(line.right), "%s", info.hasPriceLevel ? PriceSubscriber::ShortLevelName(info.priceLevel) : "");
    });
}

void DishwasherManager::RefreshClockScreens()
{
    if (mScreen != Screen::kSettings && mScreen != Screen::kPricing)
    {
        return;
    }

    // Redraw as the minute changes, so the clock moves on and ended prices drop off.
    uint32_t now = 0;
    if (WallClock::Instance().GetMatterEpochSeconds(now) && now / 60 != mShownMinute)
    {
        UpdateDishwasherDisplay();
    }
}

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

    char costBuffer[40];
    PriceSubscriber::Instance().FormatCurrentCost(costBuffer, sizeof(costBuffer));

    // The time and whether there's a price, for Settings.
    uint32_t now       = 0;
    char clockText[16] = "No time";
    const bool hasNow  = WallClock::Instance().GetMatterEpochSeconds(now);
    if (hasNow)
    {
        mShownMinute               = now / 60;
        const uint32_t secondOfDay = now % (24 * 60 * 60);
        snprintf(clockText, sizeof(clockText), "%02u:%02u UTC", static_cast<unsigned>(secondOfDay / 3600),
                 static_cast<unsigned>((secondOfDay / 60) % 60));
    }

    const char * pricingSummary = "None";
    PriceSubscriber::PriceInfo currentPrice;
    if (PriceSubscriber::Instance().GetCurrentPrice(currentPrice) && !HasEnded(currentPrice, hasNow, now))
    {
        if (currentPrice.hasPriceLevel)
        {
            pricingSummary = PriceSubscriber::ShortLevelName(currentPrice.priceLevel);
        }
        else if (currentPrice.hasPrice)
        {
            pricingSummary = "Live";
        }
    }

    char priceUnit[16];
    PriceSubscriber::Instance().FormatUnit(priceUnit, sizeof(priceUnit));
    if (mScreen == Screen::kPricing)
    {
        BuildPriceList();
    }

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
    view.costText            = costBuffer;
    view.delaySavingText     = mDelaySavingText;
    if (mScreen == Screen::kReadyBy)
    {
        RefreshReadyBy();
    }
    view.readyByS            = mReadyByS;
    view.readyByClock        = mReadyByAt;
    if (mScreen == Screen::kSmartStart)
    {
        // When it will finish: the start plus the run time.
        view.readyByClock = hasNow ? now + mDelayedStartTimeRemaining + mRunningTimeRemaining : 0;
    }
    view.hasClock            = view.readyByClock != 0;
    view.cheapSpans          = mCheapSpans;
    view.cheapSpanCount      = mCheapSpanCount;
    // When opted in, an energy manager may move the start, so there's no plan to show.
    view.hasPlannedRun       = mReadyByHasStart && !mOptedIntoEnergyManagement;
    view.plannedRun          = { mReadyByDelayS, mReadyByDelayS + GetProgramDuration() };
    view.selectedMenuItem    = mMenuItem;
    view.qrCode              = mQrCode;
    view.manualCode          = mManualCode;
    view.optimizingDots      = static_cast<uint8_t>((kOptimizingS - mHoldTicksLeft) % 3 + 1);
    view.selectedStartChoice = mSelectedStartChoice;
    view.savingText          = mSavingText;
    view.clockText           = clockText;
    view.pricingSummary      = pricingSummary;
    view.priceUnit           = priceUnit;
    view.priceLines          = mPriceLines;
    view.priceLineCount      = mPriceLineCount;
    view.priceSelectedLine   = static_cast<uint8_t>(mPriceCursor - mPriceTop);
    view.priceListCount      = mPriceListCount;
    view.priceListTop        = mPriceTop;

    StatusDisplay::Instance().UpdateDisplay(view);
}
