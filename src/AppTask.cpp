/*
 *
 *    Copyright (c) 2024 Project CHIP Authors
 *    Copyright (c) 2019 Google LLC.
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "AppTask.h"

#include "AppConfig.h"
#include "AppEvent.h"
#include "DishwasherManager.h"
#include "EnergyManagement.h"
#include "PriceSubscriber.h"
#include "RotaryEncoder.h"
#include "ShellCommands.h"
#include "WallClock.h"

#include <platform/CHIPDeviceLayer.h>
#include <platform/silabs/platformAbstraction/SilabsPlatform.h>
#include <system/SystemClock.h>

#define APP_FUNCTION_BUTTON 0
#define APP_CONTROL_BUTTON 1

using namespace chip;
using namespace chip::DeviceLayer;
using namespace chip::DeviceLayer::Silabs;

namespace {
constexpr uint64_t kLongPressMs = 1000;

uint64_t sFunctionPressedAtMs = 0;
uint64_t sControlPressedAtMs  = 0;

uint64_t NowMs()
{
    return System::SystemClock().GetMonotonicMilliseconds64().count();
}

bool IsPressed(const AppEvent * aEvent)
{
    return aEvent->ButtonEvent.Action == static_cast<uint8_t>(SilabsPlatform::ButtonAction::ButtonPressed);
}

// The encoder callbacks run in interrupt context, so they only post to the app task.
void PostEncoderEvent(EventHandler handler)
{
    AppEvent event = {};
    event.Type     = AppEvent::kEventType_Button;
    event.Handler  = handler;
    AppTask::GetAppTask().PostEvent(&event);
}

void EncoderTurned(int8_t direction)
{
    PostEncoderEvent(direction > 0 ? AppTask::EncoderClockwiseHandler : AppTask::EncoderCounterClockwiseHandler);
}

void EncoderClicked()
{
    PostEncoderEvent(AppTask::EncoderClickHandler);
}
} // namespace

AppTask AppTask::sAppTask;

CHIP_ERROR AppTask::AppInit()
{
    GetPlatform().SetButtonsCb(AppTask::ButtonEventHandler);
    RotaryEncoder::Init(EncoderTurned, EncoderClicked);

    PlatformMgr().LockChipStack();

    WallClock::Instance().Init();

    CHIP_ERROR err = app::Clusters::DeviceEnergyManagement::EnergyManagementInit();
    if (err == CHIP_NO_ERROR)
    {
        err = DishwasherMgr().Init();
    }
    if (err == CHIP_NO_ERROR)
    {
        PriceSubscriber::Instance().Init();
    }

    PlatformMgr().UnlockChipStack();

    if (err != CHIP_NO_ERROR)
    {
        SILABS_LOG("Dishwasher init failed: %" CHIP_ERROR_FORMAT, err.Format());
        appError(err);
    }

    RegisterDishwasherShellCommands();

#ifdef DISPLAY_ENABLED
#ifdef QR_CODE_ENABLED
    if (!ConnectivityMgr().IsThreadProvisioned())
    {
        GetLCD().ShowQRCode(true);
    }
#endif // QR_CODE_ENABLED
#endif

    return err;
}

CHIP_ERROR AppTask::StartAppTask()
{
    return BaseApplication::StartAppTask(AppTaskMain);
}

void AppTask::AppTaskMain(void * pvParameter)
{
    AppEvent event;
    osMessageQueueId_t sAppEventQueue = *(static_cast<osMessageQueueId_t *>(pvParameter));

    CHIP_ERROR err = sAppTask.Init();
    if (err != CHIP_NO_ERROR)
    {
        SILABS_LOG("AppTask.Init() failed");
        appError(err);
    }

#if !(defined(CHIP_CONFIG_ENABLE_ICD_SERVER) && CHIP_CONFIG_ENABLE_ICD_SERVER)
    sAppTask.StartStatusLEDTimer();
#endif

    SILABS_LOG("App Task started");

    while (true)
    {
        osStatus_t eventReceived = osMessageQueueGet(sAppEventQueue, &event, NULL, osWaitForever);
        while (eventReceived == osOK)
        {
            sAppTask.DispatchEvent(&event);
            eventReceived = osMessageQueueGet(sAppEventQueue, &event, NULL, 0);
        }
    }
}

// Called from the button driver; just queue the event for the app task.
void AppTask::ButtonEventHandler(uint8_t button, uint8_t btnAction)
{
    AppEvent buttonEvent           = {};
    buttonEvent.Type               = AppEvent::kEventType_Button;
    buttonEvent.ButtonEvent.Action = btnAction;

    if (button == APP_FUNCTION_BUTTON)
    {
        buttonEvent.Handler = FunctionButtonHandler;
    }
    else if (button == APP_CONTROL_BUTTON)
    {
        buttonEvent.Handler = ControlButtonHandler;
    }
    else
    {
        return;
    }

    AppTask::GetAppTask().PostEvent(&buttonEvent);
}

void AppTask::FunctionButtonHandler(AppEvent * aEvent)
{
    if (IsPressed(aEvent))
    {
        sFunctionPressedAtMs = NowMs();
    }
    else if (sIsProvisioned && !sIsFactoryResetTriggered && NowMs() - sFunctionPressedAtMs < kLongPressMs)
    {
        TEMPORARY_RETURN_IGNORED PlatformMgr().ScheduleWork([](intptr_t) { DishwasherMgr().SelectNextMode(); });
    }

    // Commissioning window, QR code and factory reset.
    BaseApplication::ButtonHandler(aEvent);
}

void AppTask::ControlButtonHandler(AppEvent * aEvent)
{
    if (IsPressed(aEvent))
    {
        sControlPressedAtMs = NowMs();
        return;
    }

    if (aEvent->ButtonEvent.Action != static_cast<uint8_t>(SilabsPlatform::ButtonAction::ButtonReleased))
    {
        return;
    }

    if (NowMs() - sControlPressedAtMs >= kLongPressMs)
    {
        TEMPORARY_RETURN_IGNORED PlatformMgr().ScheduleWork([](intptr_t) { DishwasherMgr().HandlePowerButton(); });
    }
    else
    {
        TEMPORARY_RETURN_IGNORED PlatformMgr().ScheduleWork([](intptr_t) { DishwasherMgr().HandleStartButton(); });
    }
}

void AppTask::EncoderClockwiseHandler(AppEvent *)
{
    TEMPORARY_RETURN_IGNORED PlatformMgr().ScheduleWork([](intptr_t) { DishwasherMgr().HandleWheelTurned(1); });
}

void AppTask::EncoderCounterClockwiseHandler(AppEvent *)
{
    TEMPORARY_RETURN_IGNORED PlatformMgr().ScheduleWork([](intptr_t) { DishwasherMgr().HandleWheelTurned(-1); });
}

void AppTask::EncoderClickHandler(AppEvent *)
{
    TEMPORARY_RETURN_IGNORED PlatformMgr().ScheduleWork([](intptr_t) { DishwasherMgr().HandleWheelClicked(); });
}
