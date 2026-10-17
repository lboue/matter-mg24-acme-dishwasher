#pragma once

#include <stdint.h>

/**
 * Shows the dishwasher state on a 128x64 SSD1306 OLED on the expansion header (see Ssd1306.h),
 * like the ESP32 version. The state is also shown on LED1 and written to the log, so the device
 * is still usable without the display attached:
 *
 *   off                  LED off
 *   on, idle             short blip every 2s
 *   delayed start        fast blink
 *   running              solid
 *   paused               slow blink
 *
 * The interface mirrors the ESP32 StatusDisplay.
 */
class StatusDisplay
{
public:
    static StatusDisplay & Instance() { return sInstance; }

    void Init();

    void TurnOn();
    void TurnOff();

    void UpdateDisplay(bool showingMenu, bool hasOptedIn, bool isProgramSelected, uint32_t startsIn, uint32_t timeRemaining,
                       const char * stateText, const char * modeText, const char * phaseText, const char * priceText);

private:
    static StatusDisplay sInstance;

    bool mIsOn = false;
};
