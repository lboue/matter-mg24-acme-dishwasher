#pragma once

#include "StatusDisplay.h"

#include <app/clusters/operational-state-server/operational-state-server.h>
#include <app/data-model/Nullable.h>
#include <lib/core/CHIPError.h>
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

    enum class StartOption : uint8_t
    {
        kNow,
        kCheapest,
        kGreenest,
    };

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
    void HandleWheelLongPressed();

    // Power (driven by the On/Off attribute)
    void TurnOnPower();
    void TurnOffPower();
    bool IsPoweredOn() const { return mIsPoweredOn; }

    // Program control (driven by Operational State commands, the encoder and the shell).
    // StartProgram() is the Matter Start command: when opted in, it waits a minute so an energy
    // manager can move the start.
    void StartProgram();
    void StartProgram(StartOption option, uint32_t delayS);
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
    void OpenStartChoice();
    bool FindCheapestStart(uint32_t & delayS) const;
    bool FindGreenestStart(uint32_t & delayS) const;
    uint32_t GetProgramDuration() const;
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

    StatusDisplay::StartChoice mStartChoices[StatusDisplay::kMaxStartChoices];
    StartOption mStartOptions[StatusDisplay::kMaxStartChoices];
    uint8_t mStartChoiceCount    = 0;
    uint8_t mSelectedStartChoice = 0;
};

inline DishwasherManager & DishwasherMgr()
{
    return DishwasherManager::sDishwasher;
}
