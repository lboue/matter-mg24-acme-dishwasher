#include "StatusDisplay.h"

#include "AppTask.h"
#include "LEDWidget.h"
#include "Ssd1306.h"
#include "qrcodegen.h"
#include "sl_i2c_instances.h"

#include <lib/support/logging/CHIPLogging.h>

#include <algorithm>
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

// A countdown in large text, centred, shrinking if it's hours long.
void DrawLargeCountdown(uint32_t seconds)
{
    char countdown[16];
    FormatDuration(seconds, countdown, sizeof(countdown));

    uint8_t scale = 3;
    while (scale > 1 && Ssd1306::TextWidth(countdown, scale) > Ssd1306::kWidth)
    {
        scale--;
    }
    sOled.DrawText((Ssd1306::kWidth - Ssd1306::TextWidth(countdown, scale)) / 2, kCountdownY, countdown, scale);
}

void DrawDelayedStart(bool hasOptedIn, uint32_t startsIn, const char * modeText, const char * priceText)
{
    DrawTitleBar("STARTING IN", hasOptedIn ? "EM" : nullptr);
    DrawLargeCountdown(startsIn);
    sOled.DrawText(0, kLine6Y, modeText);
    sOled.DrawText(0, kLine7Y, priceText);
}

void DrawStatus(bool hasOptedIn, uint32_t timeRemaining, const char * stateText, const char * modeText, const char * phaseText,
                const char * costText, uint32_t programDurationS)
{
    DrawTitleBar(stateText, hasOptedIn ? "EM" : nullptr);

    const uint8_t modeScale = Ssd1306::TextWidth(modeText, 2) <= Ssd1306::kWidth ? 2 : 1;
    sOled.DrawText(0, kModeY, modeText, modeScale);

    sOled.DrawText(0, kPhaseY, phaseText);

    // The time left while a program runs, otherwise how long the selected program takes.
    char time[24] = "";
    if (timeRemaining > 0)
    {
        FormatDuration(timeRemaining, time, sizeof(time));
        strncat(time, " left", sizeof(time) - strlen(time) - 1);
    }
    else if (programDurationS > 0)
    {
        FormatDuration(programDurationS, time, sizeof(time));
        strncat(time, " run time", sizeof(time) - strlen(time) - 1);
    }
    sOled.DrawText(0, kLine6Y, time);

    sOled.DrawText(0, kLine7Y, costText);
}

// One full-width menu row, with optional text at the right. The selected row is inverted.
void DrawMenuRow(uint8_t index, const char * left, const char * right, bool selected)
{
    constexpr int kFirstRowY = 14;
    constexpr int kRowHeight = 11;

    const int y = kFirstRowY + index * kRowHeight;
    if (selected)
    {
        sOled.FillRect(0, y - 1, Ssd1306::kWidth, Ssd1306::kCharHeight + 2, true);
    }
    sOled.DrawText(2, y, left, 1, !selected);
    if (right != nullptr)
    {
        sOled.DrawText(Ssd1306::kWidth - 2 - Ssd1306::TextWidth(right), y, right, 1, !selected);
    }
}

// A box with centred text, filled and inverted when selected.
void DrawBox(int x, int y, int width, const char * text, bool selected)
{
    constexpr int kPadding = 4;
    const int height       = Ssd1306::kCharHeight + 2 * kPadding;

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
    sOled.DrawText(x + (width - Ssd1306::TextWidth(text)) / 2, y + kPadding, text, 1, !selected);
}


// e.g. "Ready by 17:45 UTC", from Matter epoch seconds.
void FormatReadyClock(const char * label, uint32_t clock, char * buffer, size_t size)
{
    const uint32_t secondOfDay = clock % (24 * 60 * 60);
    snprintf(buffer, size, "%s %02u:%02u UTC", label, static_cast<unsigned>(secondOfDay / 3600),
             static_cast<unsigned>((secondOfDay / 60) % 60));
}

// e.g. "START - ECO 50°". Only ASCII letters are capitalised, so the degree sign survives.
void DrawStartTitle(const char * modeText)
{
    char title[32];
    snprintf(title, sizeof(title), "START - %s", modeText);
    for (char * c = title; *c != '\0'; c++)
    {
        if (*c >= 'a' && *c <= 'z')
        {
            *c = static_cast<char>(*c - 'a' + 'A');
        }
    }
    DrawTitleBar(title, nullptr);
}

// Now and Smart, side by side in boxes.
void DrawStartChoice(const char * modeText, uint8_t selected)
{
    constexpr int kMargin   = 4;
    constexpr int kBoxY     = 24;
    constexpr int kBoxWidth = (Ssd1306::kWidth - 3 * kMargin) / 2;

    DrawStartTitle(modeText);
    DrawBox(kMargin, kBoxY, kBoxWidth, "Now", selected == 0);
    DrawBox(2 * kMargin + kBoxWidth, kBoxY, kBoxWidth, "Smart", selected == 1);
}

// When the program must be ready, with a timeline of the next day: the cheapest periods dotted
// above it, the planned run solid on top of them, and an arrow under it at Ready By.
void DrawReadyBy(const StatusDisplay::View & view)
{
    constexpr int kLineLeft  = 2;
    constexpr int kLineRight = Ssd1306::kWidth - 3;
    constexpr int kLineY     = 46;
    constexpr int kBandTop   = 38;
    constexpr int kBandRows  = 6;
    constexpr uint32_t kDayS = 24 * 60 * 60;

    auto toX = [](uint32_t secondsFromNow) {
        return kLineLeft +
            static_cast<int>(static_cast<uint64_t>(secondsFromNow < kDayS ? secondsFromNow : kDayS) * (kLineRight - kLineLeft) / kDayS);
    };

    DrawStartTitle(view.modeText);

    // A UTC clock time when we know the time, otherwise how long from now.
    char readyText[24];
    if (view.hasClock)
    {
        FormatReadyClock("Ready by", view.readyByClock, readyText, sizeof(readyText));
    }
    else
    {
        snprintf(readyText, sizeof(readyText), "Ready in %u:%02u", static_cast<unsigned>(view.readyByS / 3600),
                 static_cast<unsigned>((view.readyByS / 60) % 60));
    }
    sOled.DrawText(0, 16, readyText);
    sOled.DrawText(0, 28, view.savingText);

    // The cheapest periods, lightly dotted (every other pixel of every other row).
    for (uint8_t i = 0; i < view.cheapSpanCount; i++)
    {
        const int left  = toX(view.cheapSpans[i].startS);
        const int right = std::max(toX(view.cheapSpans[i].endS) - 1, left);
        for (int x = left; x <= right; x++)
        {
            for (int y = kBandTop; y < kBandTop + kBandRows; y++)
            {
                if (x % 2 == 0 && y % 2 == 0)
                {
                    sOled.FillRect(x, y, 1, 1, true);
                }
            }
        }
    }

    // The planned run, solid, so turning the knob shows it moving into the cheap periods.
    if (view.hasPlannedRun)
    {
        const int left  = toX(view.plannedRun.startS);
        const int right = std::max(toX(view.plannedRun.endS) - 1, left);
        sOled.FillRect(left, kBandTop, right - left + 1, kBandRows, true);
    }

    // The timeline, now at the left and a day ahead at the right, with a tick at each end.
    sOled.FillRect(kLineLeft, kLineY, kLineRight - kLineLeft + 1, 1, true);
    sOled.FillRect(kLineLeft, kLineY - 2, 1, 5, true);
    sOled.FillRect(kLineRight, kLineY - 2, 1, 5, true);

    // An arrow under the line, pointing up at Ready By.
    const int arrowX = toX(view.readyByS);
    for (int row = 0; row < 3; row++)
    {
        sOled.FillRect(arrowX - row, kLineY + 3 + row, 2 * row + 1, 1, true);
    }
}

void DrawOptimizing(uint8_t dots)
{
    DrawTitleBar("SMART START", nullptr);

    // Always measured with three dots, so the text doesn't shift as they animate.
    char text[16] = "Optimizing";
    const int x   = (Ssd1306::kWidth - Ssd1306::TextWidth("Optimizing...")) / 2;
    strncat(text, "...", std::min<size_t>(dots, 3));
    sOled.DrawText(x, 28, text);
}

// How long until it starts, large, then when it will finish and what that saves.
void DrawSmartStart(uint32_t startsIn, bool hasReadyAt, uint32_t readyAtClock, const char * savingText)
{
    DrawTitleBar("SMART START", nullptr);
    DrawLargeCountdown(startsIn);

    if (hasReadyAt)
    {
        char readyText[24];
        FormatReadyClock("Ready at", readyAtClock, readyText, sizeof(readyText));
        sOled.DrawText(0, kLine6Y, readyText);
    }

    sOled.DrawText(0, kLine7Y, savingText);
}

void DrawSettings(uint8_t selected, bool hasOptedIn)
{
    DrawTitleBar("SETTINGS", nullptr);

    DrawMenuRow(StatusDisplay::kSettingsCommissioning, "Commissioning", nullptr,
                selected == StatusDisplay::kSettingsCommissioning);
    DrawMenuRow(StatusDisplay::kSettingsEnergy, "Energy Mgmt", hasOptedIn ? "In" : "Out",
                selected == StatusDisplay::kSettingsEnergy);
    DrawMenuRow(StatusDisplay::kSettingsBack, "Back", nullptr, selected == StatusDisplay::kSettingsBack);
}

void DrawEnergyMenu(uint8_t selected)
{
    DrawTitleBar("ENERGY MGMT", nullptr);

    DrawMenuRow(StatusDisplay::kEnergyOptIn, "Opt In", nullptr, selected == StatusDisplay::kEnergyOptIn);
    DrawMenuRow(StatusDisplay::kEnergyOptOut, "Opt Out", nullptr, selected == StatusDisplay::kEnergyOptOut);

    sOled.DrawText(0, kLine7Y, "click:choose");
}

// The QR code fills the left of the screen, with the manual pairing code to its right.
void DrawCommissioning(const char * qrCode, const char * manualCode)
{
    constexpr int kQuietModules = 1;
    constexpr int kMaxVersion   = 5;

    static uint8_t sQr[qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVersion)];
    static uint8_t sTemp[qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVersion)];

    int textX = 0;
    if (qrcodegen_encodeText(qrCode, sTemp, sQr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, kMaxVersion, qrcodegen_Mask_AUTO,
                             true))
    {
        const int size  = qrcodegen_getSize(sQr);
        const int scale = Ssd1306::kHeight / (size + 2 * kQuietModules);
        const int side  = (size + 2 * kQuietModules) * scale;
        const int top   = (Ssd1306::kHeight - side) / 2;

        // Dark modules on a lit square, so it reads like a printed code.
        sOled.FillRect(0, top, side, side, true);
        for (int y = 0; y < size; y++)
        {
            for (int x = 0; x < size; x++)
            {
                if (qrcodegen_getModule(sQr, x, y))
                {
                    sOled.FillRect((x + kQuietModules) * scale, top + (y + kQuietModules) * scale, scale, scale, false);
                }
            }
        }
        textX = side + 2;
    }

    // The 11-digit manual code, split 4-3-4 as it's usually printed.
    char lines[3][5]      = {};
    const size_t length   = strlen(manualCode);
    const size_t splits[] = { 0, 4, 7, length };
    for (size_t i = 0; i < 3; i++)
    {
        const size_t from = std::min(splits[i], length);
        const size_t to   = std::min(splits[i + 1], length);
        memcpy(lines[i], manualCode + from, std::min<size_t>(to - from, sizeof(lines[i]) - 1));
    }

    sOled.DrawText(textX, 2, "Pairing");
    sOled.DrawText(textX, 16, lines[0]);
    sOled.DrawText(textX, 26, lines[1]);
    sOled.DrawText(textX, 36, lines[2]);
    sOled.DrawText(textX, kLine7Y, "click:stop");
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

    if (view.screen == Screen::kSettings)
    {
        static const char * const kItems[] = { "Commissioning", "Energy Mgmt", "Back" };
        snprintf(status, sizeof(status), "SETTINGS | %s | EM %s", kItems[view.selectedMenuItem], optInText);
        DrawSettings(view.selectedMenuItem, view.hasOptedIn);
    }
    else if (view.screen == Screen::kCommissioning)
    {
        snprintf(status, sizeof(status), "COMMISSIONING | %s", view.manualCode);
        DrawCommissioning(view.qrCode, view.manualCode);
    }
    else if (view.screen == Screen::kOptimizing)
    {
        snprintf(status, sizeof(status), "OPTIMIZING | %s", view.modeText);
        DrawOptimizing(view.optimizingDots);
    }
    else if (view.screen == Screen::kSmartStart)
    {
        snprintf(status, sizeof(status), "SMART START | %s | %s", view.modeText, view.savingText);
        countdown = view.startsIn;
        DrawSmartStart(view.startsIn, view.hasClock, view.readyByClock, view.savingText);
    }
    else if (view.screen == Screen::kEnergyMenu)
    {
        snprintf(status, sizeof(status), "ENERGY | %s | EM %s",
                 view.selectedMenuItem == kEnergyOptIn ? "Opt In" : "Opt Out", optInText);
        DrawEnergyMenu(view.selectedMenuItem);
    }
    else if (view.screen == Screen::kStartChoice)
    {
        snprintf(status, sizeof(status), "START | %s | %s", view.modeText, view.selectedStartChoice == 0 ? "Now" : "Smart");
        DrawStartChoice(view.modeText, view.selectedStartChoice);
    }
    else if (view.screen == Screen::kReadyBy)
    {
        snprintf(status, sizeof(status), "READY BY | %s | in %lus | %s", view.modeText, static_cast<unsigned long>(view.readyByS),
                 view.savingText);
        DrawReadyBy(view);
    }
    else if (view.isProgramSelected && view.startsIn > 0)
    {
        sStateLED.Blink(100, 100);
        snprintf(status, sizeof(status), "Starting in | %s | EM %s | %s", view.modeText, optInText, view.priceText);
        countdown = view.startsIn;
        // After Now picked a later start, what that saves rather than the price.
        DrawDelayedStart(view.hasOptedIn, view.startsIn, view.modeText,
                         view.delaySavingText[0] != '\0' ? view.delaySavingText : view.priceText);
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
        DrawStatus(view.hasOptedIn, view.timeRemaining, view.stateText, modeText, view.phaseText, view.costText,
                   view.isSettingsSelected ? 0 : view.programDurationS);
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
