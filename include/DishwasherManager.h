#pragma once

#include "StartPlanner.h"
#include "StatusDisplay.h"

#include <app/clusters/operational-state-server/operational-state-server.h>
#include <app/data-model/Nullable.h>
#include <lib/core/CHIPError.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>
#include <system/SystemLayer.h>

/**
 * The dishwasher itself, ported from dishwasher_manager.cpp in matter-esp32-tiny-dishwasher.
 *
 * Threading: unlike the ESP32 version (which ticked from its own FreeRTOS task and used
 * ScheduleWork/LockChipStack), every method here must be called on the Matter thread. The
 * one-second program tick is a CHIP system timer, and buttons/shell commands are marshalled
 * across with PlatformMgr().ScheduleWork().
 */
class DishwasherManager
{
public:
    using OperationalStateEnum = chip::app::Clusters::OperationalState::OperationalStateEnum;

    static constexpr chip::EndpointId kDishwasherEndpoint = 1;

    CHIP_ERROR Init();

    // Physical controls
    void HandlePowerButton();
    void HandleStartButton();
    void HandleCancel();
    void SelectMode(uint8_t mode);
    void SelectNextMode();
    void SetEnergyManagementOptIn(bool optIn);
    void HandleWheelTurned(int8_t direction);
    void HandleWheelClicked();
    void HandleCancelButton();

    // Power (driven by the On/Off attribute)
    void TurnOnPower();
    void TurnOffPower();
    bool IsPoweredOn() const { return mIsPoweredOn; }

    // Program control (driven by Operational State commands, the encoder and the shell).
    // StartProgram() is the Matter Start command: when opted in, it waits a minute so an energy
    // manager can move the start.
    void StartProgram();
    void StopProgram();
    void PauseProgram();
    void ResumeProgram();

    void UpdateMode(uint8_t mode);
    CHIP_ERROR AdjustStartTime(uint32_t newStartTime);

    OperationalStateEnum GetOperationalState() const { return mState; }
    uint8_t GetCurrentMode() const { return mMode; }
    bool IsOptedIntoEnergyManagement() const { return mOptedIntoEnergyManagement; }
    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() const;

    void UpdateDishwasherDisplay();

private:
    friend DishwasherManager & DishwasherMgr();
    static DishwasherManager sDishwasher;

    static void ProgramTick(chip::System::Layer * layer, void * context);

    void ToggleProgram();
    void BeginProgram(uint32_t delayS, bool canMoveStart);
    void OpenReadyBy();
    void SetReadyByFromNow(uint32_t secondsFromNow);
    void RefreshReadyBy();
    uint32_t EarliestReadyByS() const;
    void UpdateReadyBy();
    void StartReadyBy();
    void ShowScreen(StatusDisplay::Screen screen, uint8_t menuItem = 0);
    void OpenCommissioning();
    void CloseCommissioning();
    void CheckCommissioningWindow();
    bool BuildPricePeriods(chip::Span<const StartPlanner::Period> & periods, uint32_t & now) const;
    bool BuildGridPeriods(chip::Span<const StartPlanner::Period> & periods, uint32_t & now) const;

    void TickOptimizing();
    void ShowSmartStart();
    void CancelSmartStart();
    void UpdateSmartSaving();
    void FormatSaving(chip::Span<const StartPlanner::Period> periods, uint32_t now, uint32_t delayS);
    uint32_t GetProgramDuration() const;
    chip::Span<const StartPlanner::Step> GetPowerProfile() const;
    void ProgressProgram();
    void EndProgram();
    void UpdateOperationState(OperationalStateEnum state);
    void UpdateCurrentPhase(uint8_t phase);
    void SetForecast();
    void ClearForecast();

    OperationalStateEnum mState = OperationalStateEnum::kStopped;
    uint8_t mMode               = 0;
    uint8_t mPhase              = 0;
    uint32_t mRunningTimeRemaining      = 0;
    uint32_t mDelayedStartTimeRemaining = 0;
    bool mOptedIntoEnergyManagement     = false;

    uint32_t mCurrentForecastId = 0;

    bool mIsProgramSelected = false;
    bool mIsPoweredOn       = false;

    // Whether the DEM forecast offers an energy manager a window to move the start in.
    bool mCanMoveStart = false;

    StatusDisplay::Screen mScreen = StatusDisplay::Screen::kStatus;

    // The Settings item after the programs on the main view. It isn't a mode, so mMode keeps the
    // last program.
    bool mIsSettingsSelected = false;

    // The Ready By screen: how long from now the program must finish, and the best start within
    // that, which a click uses.
    uint32_t mReadyByS      = 0;
    uint32_t mReadyByAt     = 0; // The same as a clock time on a quarter hour, or 0 without the clock
    bool mReadyByHasStart   = false;
    uint32_t mReadyByDelayS = 0;
    bool mSmartUsesPrice    = false; // Whether that start came from the price, or the grid conditions

    // The deadline of a program started from Ready By, in Matter epoch seconds, or 0 for none. The
    // DEM forecast's latestEndTime, and the limit for energy manager adjustments.
    uint32_t mReadyByEpoch = 0;

    uint8_t mOptimizingTicksLeft = 0;
    bool mHoldingStart           = false; // While Optimizing: don't start yet, so an energy manager can move it
    char mSavingText[40]         = "";

    // The highlighted row on the Settings and Energy Mgmt pages.
    uint8_t mMenuItem = 0;

    // The onboarding codes shown on the Commissioning page.
    char mQrCode[chip::QRCodeBasicSetupPayloadGenerator::kMaxQRCodeBase38RepresentationLength + 1] = "";
    char mManualCode[chip::kManualSetupLongCodeCharLength + 1]                                    = "";
};

inline DishwasherManager & DishwasherMgr()
{
    return DishwasherManager::sDishwasher;
}
