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
    enum class Screen : uint8_t
    {
        kStatus,        // The normal screen, or the delayed-start countdown
        kSettings,      // Commissioning / Energy Mgmt / Exit
        kCommissioning, // The QR code and manual pairing code, while the commissioning window is open
        kEnergyMenu,    // Opt In / Opt Out, opened from Settings
        kStartChoice,   // Now / Cheapest / Greenest
    };

    struct StartChoice
    {
        const char * label = "";
        uint32_t delayS    = 0;
    };

    static constexpr uint8_t kMaxStartChoices = 3;

    // The rows on the Settings page, and on the Energy Mgmt page.
    enum SettingsItem : uint8_t
    {
        kSettingsCommissioning,
        kSettingsEnergy,
        kSettingsExit,
        kSettingsItemCount,
    };
    enum EnergyItem : uint8_t
    {
        kEnergyOptIn,
        kEnergyOptOut,
        kEnergyItemCount,
    };

    struct View
    {
        Screen screen             = Screen::kStatus;
        bool hasOptedIn           = false;
        bool isProgramSelected    = false;
        bool isSettingsSelected   = false; // The Settings item on the program list, not a program
        uint32_t startsIn         = 0;
        uint32_t timeRemaining    = 0;
        uint32_t programDurationS = 0; // The selected program's full running time
        const char * stateText    = "";
        const char * modeText     = "";
        const char * phaseText    = "";
        const char * priceText    = "";

        const StartChoice * startChoices = nullptr;
        uint8_t startChoiceCount         = 0;
        uint8_t selectedStartChoice      = 0;

        uint8_t selectedMenuItem = 0; // On kSettings and kEnergyMenu

        const char * qrCode     = ""; // On kCommissioning
        const char * manualCode = "";
    };

    static StatusDisplay & Instance() { return sInstance; }

    void Init();

    void TurnOn();
    void TurnOff();

    void UpdateDisplay(const View & view);

private:
    static StatusDisplay sInstance;

    bool mIsOn = false;
};
