#include "StatusDisplay.h"

#include "AppTask.h"
#include "LEDWidget.h"
#include "Ssd1306.h"
#include "sl_i2c_instances.h"

#include <lib/support/logging/CHIPLogging.h>

#include <stdio.h>
#include <string.h>

#ifdef SL_CATALOG_SIMPLE_LED_LED1_PRESENT
#define DISHWASHER_STATE_LED 1
#else
#define DISHWASHER_STATE_LED 0
#endif

namespace {
LEDWidget sStateLED;

// Only log when something other than the countdown changes, plus a heartbeat while counting down.
constexpr uint32_t kCountdownLogIntervalS = 30;
char sLastStatus[128];

Ssd1306 sOled;

// Text rows on the 128x64 panel. The title bar is inverted, 9px tall.
constexpr int kTitleBarHeight = 9;
constexpr int kTitleY         = 1;
constexpr int kModeY          = 14;
constexpr int kCountdownY     = 16;
constexpr int kPhaseY         = 34;
constexpr int kLine6Y         = 44;
constexpr int kLine7Y         = 54;

void FormatDuration(uint32_t seconds, char * buffer, size_t size)
{
    const auto h = static_cast<unsigned>(seconds / 3600);
    const auto m = static_cast<unsigned>((seconds / 60) % 60);
    const auto s = static_cast<unsigned>(seconds % 60);
    if (h > 0)
    {
        snprintf(buffer, size, "%u:%02u:%02u", h, m, s);
    }
    else
    {
        snprintf(buffer, size, "%u:%02u", m, s);
    }
}

void DrawTitleBar(const char * left, const char * right)
{
    sOled.FillRect(0, 0, Ssd1306::kWidth, kTitleBarHeight, true);
    sOled.DrawText(1, kTitleY, left, 1, false);
    if (right != nullptr)
    {
        sOled.DrawText(Ssd1306::kWidth - 1 - Ssd1306::TextWidth(right), kTitleY, right, 1, false);
    }
}

void DrawDelayedStart(bool hasOptedIn, uint32_t startsIn, const char * modeText, const char * priceText)
{
    char countdown[16];
    FormatDuration(startsIn, countdown, sizeof(countdown));

    // The countdown is large, shrinking if it's hours long.
    uint8_t scale = 3;
    while (scale > 1 && Ssd1306::TextWidth(countdown, scale) > Ssd1306::kWidth)
    {
        scale--;
    }

    DrawTitleBar("STARTING IN", hasOptedIn ? "EM" : nullptr);
    sOled.DrawText((Ssd1306::kWidth - Ssd1306::TextWidth(countdown, scale)) / 2, kCountdownY, countdown, scale);
    sOled.DrawText(0, kLine6Y, modeText);
    sOled.DrawText(0, kLine7Y, priceText);
}

void DrawStatus(bool hasOptedIn, uint32_t timeRemaining, const char * stateText, const char * modeText, const char * phaseText,
                const char * priceText, const char * hint)
{
    DrawTitleBar(stateText, hasOptedIn ? "EM" : nullptr);

    const uint8_t modeScale = Ssd1306::TextWidth(modeText, 2) <= Ssd1306::kWidth ? 2 : 1;
    sOled.DrawText(0, kModeY, modeText, modeScale);

    sOled.DrawText(0, kPhaseY, phaseText);

    // The time left while a program runs. Settings can only be selected when none is, so its hint
    // shares the row.
    if (timeRemaining > 0)
    {
        char remaining[24];
        FormatDuration(timeRemaining, remaining, sizeof(remaining));
        strncat(remaining, " left", sizeof(remaining) - strlen(remaining) - 1);
        sOled.DrawText(0, kLine6Y, remaining);
    }
    else if (hint != nullptr)
    {
        sOled.DrawText(0, kLine6Y, hint);
    }

    sOled.DrawText(0, kLine7Y, priceText);
}

// Draws one menu choice in a box, inverted when selected.
void DrawMenuOption(int x, const char * text, bool selected)
{
    constexpr int kPadding = 3;
    const int width        = Ssd1306::TextWidth(text) + 2 * kPadding;
    const int height       = Ssd1306::kCharHeight + 2 * kPadding;
    const int y            = kModeY + 8;

    if (selected)
    {
        sOled.FillRect(x, y, width, height, true);
    }
    else
    {
        sOled.FillRect(x, y, width, 1, true);
        sOled.FillRect(x, y + height - 1, width, 1, true);
        sOled.FillRect(x, y, 1, height, true);
        sOled.FillRect(x + width - 1, y, 1, height, true);
    }
    sOled.DrawText(x + kPadding, y + kPadding, text, 1, !selected);
}

void DrawMenu(bool hasOptedIn)
{
    DrawTitleBar("ENERGY MGR", nullptr);

    // Opt Out on the left and Opt In on the right, matching the turn direction.
    DrawMenuOption(4, "Opt Out", !hasOptedIn);
    const int optInWidth = Ssd1306::TextWidth("Opt In") + 6;
    DrawMenuOption(Ssd1306::kWidth - 4 - optInWidth, "Opt In", hasOptedIn);

    sOled.DrawText(0, kLine7Y, "turn: set click: exit");
}

// One row per choice, with its delay at the right. The selected row is inverted.
void DrawStartChoice(const StatusDisplay::StartChoice * choices, uint8_t count, uint8_t selected)
{
    constexpr int kFirstRowY = 14;
    constexpr int kRowHeight = 11;

    DrawTitleBar("START", nullptr);

    for (uint8_t i = 0; i < count; i++)
    {
        const int y     = kFirstRowY + i * kRowHeight;
        const bool isOn = i != selected;
        char delay[16]  = "";
        if (i == selected)
        {
            sOled.FillRect(0, y - 1, Ssd1306::kWidth, Ssd1306::kCharHeight + 2, true);
        }
        sOled.DrawText(2, y, choices[i].label, 1, isOn);

        if (choices[i].delayS > 0)
        {
            delay[0] = '+';
            FormatDuration(choices[i].delayS, delay + 1, sizeof(delay) - 1);
        }
        else if (i > 0)
        {
            snprintf(delay, sizeof(delay), "now");
        }
        sOled.DrawText(Ssd1306::kWidth - 2 - Ssd1306::TextWidth(delay), y, delay, 1, isOn);
    }

    sOled.DrawText(0, kLine7Y, "click:start hold:back");
}
} // namespace

StatusDisplay StatusDisplay::sInstance;

void StatusDisplay::Init()
{
    sStateLED.Init(DISHWASHER_STATE_LED);
    // Lets the Silabs base application blink this LED for Identify and factory reset.
    AppTask::GetAppTask().LinkAppLed(&sStateLED);

    // Starts off, matching the dead front. The display stays blank until TurnOn().
    sOled.Init(sl_i2c_qwiic_handle);
}

void StatusDisplay::TurnOn()
{
    ChipLogProgress(AppServer, "[display] on");
    mIsOn = true;
    sOled.SetPower(true);
}

void StatusDisplay::TurnOff()
{
    ChipLogProgress(AppServer, "[display] off");
    mIsOn = false;
    sStateLED.Set(false);
    sLastStatus[0] = '\0';

    sOled.Clear();
    sOled.Flush();
    sOled.SetPower(false);
}

void StatusDisplay::UpdateDisplay(const View & view)
{
    if (!mIsOn)
    {
        return;
    }

    char status[sizeof(sLastStatus)];
    uint32_t countdown = 0;

    const char * optInText = view.hasOptedIn ? "opt-in" : "opt-out";

    sOled.Clear();

    if (view.screen == Screen::kEnergyMenu)
    {
        snprintf(status, sizeof(status), "MENU | EM %s", optInText);
        DrawMenu(view.hasOptedIn);
    }
    else if (view.screen == Screen::kStartChoice)
    {
        snprintf(status, sizeof(status), "START | %s | %s", view.modeText,
                 view.startChoices[view.selectedStartChoice].label);
        DrawStartChoice(view.startChoices, view.startChoiceCount, view.selectedStartChoice);
    }
    else if (view.isProgramSelected && view.startsIn > 0)
    {
        sStateLED.Blink(100, 100);
        snprintf(status, sizeof(status), "Starting in | %s | EM %s | %s", view.modeText, optInText, view.priceText);
        countdown = view.startsIn;
        DrawDelayedStart(view.hasOptedIn, view.startsIn, view.modeText, view.priceText);
    }
    else
    {
        if (strcmp(view.stateText, "RUNNING") == 0)
        {
            sStateLED.Set(true);
        }
        else if (strcmp(view.stateText, "PAUSED") == 0)
        {
            sStateLED.Blink(500, 500);
        }
        else
        {
            sStateLED.Blink(100, 1900);
        }

        const char * modeText = view.isSettingsSelected ? "Settings" : view.modeText;
        snprintf(status, sizeof(status), "%s | %s | %s | EM %s | %s", view.stateText, modeText, view.phaseText, optInText,
                 view.priceText);
        countdown = view.timeRemaining;
        DrawStatus(view.hasOptedIn, view.timeRemaining, view.stateText, modeText, view.phaseText, view.priceText,
                   view.isSettingsSelected ? "click to open" : nullptr);
    }

    // Only sends the pages that changed.
    sOled.Flush();

    bool changed = strcmp(status, sLastStatus) != 0;
    if (changed || (countdown > 0 && countdown % kCountdownLogIntervalS == 0))
    {
        ChipLogProgress(AppServer, "[display] %s | %lus", status, static_cast<unsigned long>(countdown));
        strncpy(sLastStatus, status, sizeof(sLastStatus));
    }
}
