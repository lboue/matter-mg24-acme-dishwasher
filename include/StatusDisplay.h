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
        kSettings,      // Commissioning / Energy Mgmt / Back
        kCommissioning, // The QR code and manual pairing code, while the commissioning window is open
        kEnergyMenu,    // Opt In / Opt Out, opened from Settings
        kStartChoice,   // Now / Smart
        kReadyBy,       // When the program must be ready, on a timeline of the next day
        kOptimizing,    // Waiting a few seconds for an energy manager to move a scheduled start
        kSmartStart,    // When a scheduled start will begin and what it saves, with Cancel
        kPricing,       // The current and future prices, opened from Settings
    };

    // A stretch of the Ready By timeline, in seconds from now.
    struct TimelineSpan
    {
        uint32_t startS = 0;
        uint32_t endS   = 0;
    };

    static constexpr uint8_t kMaxCheapSpans = 12;

    // One row of a list, with optional text at the right.
    struct MenuLine
    {
        char left[24]  = "";
        char right[10] = "";
    };

    // The price rows that fit on the Pricing page at once.
    static constexpr uint8_t kMaxPriceLines = 4;

    // The rows on the Settings page, and on the Energy Mgmt page.
    enum SettingsItem : uint8_t
    {
        kSettingsCommissioning,
        kSettingsEnergy,
        kSettingsPricing,
        kSettingsBack,
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
        Screen screen                = Screen::kStatus;
        bool hasOptedIn              = false;
        bool isProgramSelected       = false;
        bool isSettingsSelected      = false; // The Settings item on the program list, not a program
        uint32_t startsIn            = 0;
        uint32_t timeRemaining       = 0;
        uint32_t programDurationS    = 0; // The selected program's full running time
        const char * stateText       = "";
        const char * modeText        = "";
        const char * phaseText       = "";
        const char * priceText       = "";
        const char * costText        = ""; // The price level, for the home screen
        const char * delaySavingText = ""; // While waiting to start after Now: what the delay saves

        uint8_t selectedStartChoice = 0; // On kStartChoice: 0 is Now, 1 is Smart

        uint32_t readyByS     = 0;     // On kReadyBy: how long from now it must be ready
        bool hasClock         = false; // Whether readyByClock is known
        uint32_t readyByClock = 0;     // Matter epoch seconds: Ready By, or on kSmartStart, when it will finish

        // On kReadyBy: the cheapest periods, shaded on the timeline, and the planned run, drawn solid.
        const TimelineSpan * cheapSpans = nullptr;
        uint8_t cheapSpanCount          = 0;
        bool hasPlannedRun              = false;
        TimelineSpan plannedRun;

        uint8_t selectedMenuItem = 0; // On kSettings and kEnergyMenu

        const char * clockText      = ""; // On kSettings: the time, e.g. "14:05 UTC"
        const char * pricingSummary = ""; // On kSettings: the current price level, or "None"

        // On kPricing: the visible rows, which is selected, and where they are in the whole list.
        const char * priceUnit       = "";
        const MenuLine * priceLines  = nullptr;
        uint8_t priceLineCount       = 0;
        uint8_t priceSelectedLine    = 0;
        uint8_t priceListCount       = 0;
        uint8_t priceListTop         = 0;

        uint8_t optimizingDots  = 1;  // On kOptimizing
        const char * savingText = ""; // On kReadyBy and kSmartStart

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
