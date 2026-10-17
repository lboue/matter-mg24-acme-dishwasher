#include "ShellCommands.h"

#ifdef ENABLE_CHIP_SHELL

#include "DishwasherManager.h"
#include "GridConditionsSubscriber.h"
#include "PriceSubscriber.h"
#include "WallClock.h"

#include <lib/shell/Engine.h>
#include <lib/shell/commands/Help.h>
#include <platform/CHIPDeviceLayer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace chip;
using Shell::Engine;
using Shell::shell_command_t;
using Shell::streamer_get;
using Shell::streamer_printf;

namespace {

Engine sDishwasherSubCommands;

// The shell runs on its own task; hop onto the Matter thread before touching the dishwasher.
CHIP_ERROR RunOnMatterThread(void (*fn)())
{
    return DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg) { reinterpret_cast<void (*)()>(arg)(); },
                                                   reinterpret_cast<intptr_t>(fn));
}

CHIP_ERROR HelpHandler(int argc, char ** argv)
{
    sDishwasherSubCommands.ForEachCommand(Shell::PrintCommandHelp, nullptr);
    return CHIP_NO_ERROR;
}

CHIP_ERROR PowerHandler(int argc, char ** argv)
{
    return RunOnMatterThread([] { DishwasherMgr().HandlePowerButton(); });
}

CHIP_ERROR StartHandler(int argc, char ** argv)
{
    return RunOnMatterThread([] { DishwasherMgr().HandleStartButton(); });
}

CHIP_ERROR CancelHandler(int argc, char ** argv)
{
    return RunOnMatterThread([] { DishwasherMgr().HandleCancel(); });
}

CHIP_ERROR ModeHandler(int argc, char ** argv)
{
    if (argc != 1)
    {
        streamer_printf(streamer_get(), "Usage: dishwasher mode next|0|1|2\r\n");
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    if (strcmp(argv[0], "next") == 0)
    {
        return RunOnMatterThread([] { DishwasherMgr().SelectNextMode(); });
    }

    auto mode = static_cast<intptr_t>(atoi(argv[0]));
    return DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t arg) { DishwasherMgr().SelectMode(static_cast<uint8_t>(arg)); }, mode);
}

CHIP_ERROR OptInHandler(int argc, char ** argv)
{
    if (argc != 1 || (strcmp(argv[0], "on") != 0 && strcmp(argv[0], "off") != 0))
    {
        streamer_printf(streamer_get(), "Usage: dishwasher optin on|off\r\n");
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    intptr_t optIn = strcmp(argv[0], "on") == 0;
    return DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t arg) { DishwasherMgr().SetEnergyManagementOptIn(arg != 0); }, optIn);
}

CHIP_ERROR StatusHandler(int argc, char ** argv)
{
    return RunOnMatterThread([] {
        uint32_t now = 0;
        bool synced  = WallClock::Instance().GetMatterEpochSeconds(now);
        ChipLogProgress(AppServer, "power=%s time=%s (%lu)", DishwasherMgr().IsPoweredOn() ? "on" : "off",
                        synced ? "synced" : "NOT synced", static_cast<unsigned long>(now));
        DishwasherMgr().UpdateDishwasherDisplay();
    });
}

void LogPrice(const char * label, const PriceSubscriber::PriceInfo & info)
{
    char price[40] = "-";
    if (info.hasPrice)
    {
        PriceSubscriber::Instance().FormatPrice(info.price, price, sizeof(price));
    }
    char level[16] = "";
    if (info.hasPriceLevel)
    {
        snprintf(level, sizeof(level), " level %d", info.priceLevel);
    }
    ChipLogProgress(AppServer, "  %s %lu..%lu %s%s", label, static_cast<unsigned long>(info.periodStart),
                    static_cast<unsigned long>(info.periodEnd), price, level);
}

CHIP_ERROR PriceHandler(int argc, char ** argv)
{
    return RunOnMatterThread([] {
        auto & prices = PriceSubscriber::Instance();
        if (!prices.IsBound())
        {
            ChipLogProgress(AppServer, "No Commodity Price binding. Bind with: "
                                       "chip-tool binding write binding '[{\"node\":<tariff>,\"endpoint\":<ep>,\"cluster\":149}]' "
                                       "<dishwasher> 1");
            return;
        }

        PriceSubscriber::PriceInfo current;
        if (prices.GetCurrentPrice(current))
        {
            ChipLogProgress(AppServer, "Current price (%s), times in Matter epoch seconds:", prices.GetCurrentPriceDescription());
            LogPrice("now ", current);
        }
        else
        {
            ChipLogProgress(AppServer, "Current price unknown");
        }

        if (!prices.IsForecastSupported())
        {
            ChipLogProgress(AppServer, "Forecast not supported by server");
            return;
        }

        auto forecast = prices.GetForecast();
        ChipLogProgress(AppServer, "Forecast (%u entries):", static_cast<unsigned>(forecast.size()));
        for (const auto & entry : forecast)
        {
            LogPrice("fcst", entry);
        }
    });
}

void LogConditions(const char * label, const GridConditionsSubscriber::Conditions & conditions)
{
    ChipLogProgress(AppServer, "  %s %lu..%lu %d gCO2/kWh", label, static_cast<unsigned long>(conditions.periodStart),
                    static_cast<unsigned long>(conditions.periodEnd), conditions.gridCarbonIntensity);
}

CHIP_ERROR GridHandler(int argc, char ** argv)
{
    return RunOnMatterThread([] {
        auto & grid = GridConditionsSubscriber::Instance();
        if (!grid.IsBound())
        {
            ChipLogProgress(AppServer, "No Electrical Grid Conditions binding. Bind with: "
                                       "chip-tool binding write binding '[{\"node\":<grid>,\"endpoint\":<ep>,\"cluster\":160}]' "
                                       "<dishwasher> 1");
            return;
        }

        GridConditionsSubscriber::Conditions current;
        if (grid.GetCurrentConditions(current))
        {
            ChipLogProgress(AppServer, "Current grid conditions, times in Matter epoch seconds:");
            LogConditions("now ", current);
        }
        else
        {
            ChipLogProgress(AppServer, "Current grid conditions unknown");
        }

        if (!grid.IsForecastSupported())
        {
            ChipLogProgress(AppServer, "Forecast not supported by server");
            return;
        }

        auto forecast = grid.GetForecast();
        ChipLogProgress(AppServer, "Forecast (%u entries):", static_cast<unsigned>(forecast.size()));
        for (const auto & entry : forecast)
        {
            LogConditions("fcst", entry);
        }
    });
}

CHIP_ERROR DishwasherCommandHandler(int argc, char ** argv)
{
    if (argc == 0)
    {
        return HelpHandler(argc, argv);
    }
    return sDishwasherSubCommands.ExecCommand(argc, argv);
}

} // namespace

void RegisterDishwasherShellCommands()
{
    static const shell_command_t sSubCommands[] = {
        { &HelpHandler, "help", "Usage: dishwasher <subcommand>" },
        { &PowerHandler, "power", "Toggle power (On/Off)" },
        { &StartHandler, "start", "Start / pause / resume the program" },
        { &CancelHandler, "cancel", "Cancel the current program" },
        { &ModeHandler, "mode", "Select the program: next|0 (Eco)|1 (Chef)|2 (Quick)" },
        { &OptInHandler, "optin", "Energy management opt-in: on|off" },
        { &StatusHandler, "status", "Log the current state" },
        { &PriceHandler, "price", "Log the current and forecast electricity price" },
        { &GridHandler, "grid", "Log the current and forecast grid carbon intensity" },
    };

    static const shell_command_t sDishwasherCommand = { &DishwasherCommandHandler, "dishwasher",
                                                        "Tiny dishwasher controls. Usage: dishwasher <subcommand>" };

    sDishwasherSubCommands.RegisterCommands(sSubCommands, MATTER_ARRAY_SIZE(sSubCommands));
    Engine::Root().RegisterCommands(&sDishwasherCommand, 1);
}

#else

void RegisterDishwasherShellCommands() {}

#endif // ENABLE_CHIP_SHELL
