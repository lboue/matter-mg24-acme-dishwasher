#pragma once

/**
 * Registers the `dishwasher` shell command, which stands in for the ESP32 version's rotary encoder
 * menu (mode selection, energy management opt-in, cancel) on a board with no screen.
 */
void RegisterDishwasherShellCommands();
