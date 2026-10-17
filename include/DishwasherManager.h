#pragma once

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

    // Power (driven by the On/Off attribute)
    void TurnOnPower();
    void TurnOffPower();
    bool IsPoweredOn() const { return mIsPoweredOn; }

    // Program control (driven by Operational State commands and the buttons)
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
    bool mIsShowingMenu     = false;
};

inline DishwasherManager & DishwasherMgr()
{
    return DishwasherManager::sDishwasher;
}
