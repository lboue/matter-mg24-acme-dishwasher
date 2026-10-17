/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
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

#pragma once

#include <lib/core/CHIPError.h>
#include <platform/CHIPDeviceLayer.h>

#include "BaseApplication.h"

// Application-defined error codes in the CHIP_ERROR space.
#define APP_ERROR_EVENT_QUEUE_FAILED CHIP_APPLICATION_ERROR(0x01)
#define APP_ERROR_CREATE_TASK_FAILED CHIP_APPLICATION_ERROR(0x02)
#define APP_ERROR_UNHANDLED_EVENT CHIP_APPLICATION_ERROR(0x03)
#define APP_ERROR_CREATE_TIMER_FAILED CHIP_APPLICATION_ERROR(0x04)
#define APP_ERROR_START_TIMER_FAILED CHIP_APPLICATION_ERROR(0x05)
#define APP_ERROR_STOP_TIMER_FAILED CHIP_APPLICATION_ERROR(0x06)

/**
 * Buttons on the xG24 Dev Kit (the ESP32 version had two buttons and a rotary encoder):
 *
 *   BTN0 short press   Next program (when commissioned), plus the usual Silabs behaviour:
 *                      open the commissioning window if not commissioned, and log the QR code
 *   BTN0 hold 6s       Factory reset (Silabs standard; replaces the ESP32 reset prompt)
 *   BTN1 short press   Start / pause / resume
 *   BTN1 hold 1s       Power on / off
 *
 * Energy management opt-in and cancel are on the `dishwasher` shell command.
 */
class AppTask : public BaseApplication
{
public:
    AppTask() = default;

    static AppTask & GetAppTask() { return sAppTask; }

    static void AppTaskMain(void * pvParameter);

    CHIP_ERROR StartAppTask();

    static void ButtonEventHandler(uint8_t button, uint8_t btnAction);

private:
    static AppTask sAppTask;

    CHIP_ERROR AppInit() override;

    static void FunctionButtonHandler(AppEvent * aEvent);
    static void ControlButtonHandler(AppEvent * aEvent);
};
