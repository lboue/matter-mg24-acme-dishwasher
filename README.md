# MG24 Tiny Matter Dishwasher

A port of [matter-esp32-tiny-dishwasher](../matter-esp32-tiny-dishwasher) to the Silicon Labs EFR32MG24, running Matter over Thread on the **xG24 Dev Kit (BRD2601B)**.

It implements the Matter Dishwasher device type, with the mandatory Operational State cluster, the optional On/Off (Dead Front) and Dishwasher Mode clusters, and a Device Energy Management endpoint that publishes a power forecast and accepts start-time adjustments.

It is based on the Silicon Labs `matter_thread_soc_dishwasher_app_freertos` sample (Simplicity SDK 2026.6.0, Matter extension 2.9.0). The dishwasher logic itself comes from the ESP32 version.

## Data model

| Endpoint | Device type | Clusters |
| -------- | ----------- | -------- |
| 0 | Root Node, OTA Requestor | standard, plus **Time Synchronization** (SetUTCTime only) |
| 1 | Dishwasher | Identify, Descriptor, Binding, **Operational State** (+ CountdownTime), **Dishwasher Mode**, **On/Off** (DeadFrontBehavior); **Commodity Price** and **Electrical Grid Conditions** clients |
| 2 | Device Energy Management | Descriptor, **Device Energy Management** (PFR + STA), Device Energy Management Mode |

The data model lives in `config/common/dishwasher-thread-app.zap`. Edit it with the ZAP tool from Simplicity Studio and regenerate.

## Hardware

The dev kit has two buttons, and the OLED and rotary encoder are wired to its expansion header (see [Display](#display) and [Rotary encoder](#rotary-encoder)). The ESP32's controls map onto it like this:

| Control | ESP32 | xG24 Dev Kit |
| ------- | ----- | ------------ |
| Power on/off | On/Off button | **BTN1** press |
| Select program | Rotary encoder | **Encoder** turn, **BTN0** short press (once commissioned), or `dishwasher mode` |
| Start | Start button | **Encoder** click on a program (choosing Now, Cheapest or Greenest), or `dishwasher start` |
| Pause / resume | Start button | **Encoder** click while running, or `dishwasher start` |
| Energy management opt-in | Menu (rotary click and turn) | **Encoder**: **Settings** → **Energy Mgmt**, turn to choose, click to apply; or `dishwasher optin on\|off` |
| Share with another controller | — | **Encoder**: **Settings** → **Commissioning** shows the QR and manual pairing codes |
| Cancel program | Rotary click | **Encoder** hold ≥ 1 s, or `dishwasher cancel` |
| Factory reset | Hold On/Off 5 s, then confirm | **BTN0** hold ~6 s (Silabs standard; LEDs blink, release to cancel) |
| Display | SSD1306 OLED | SH1106 OLED on the expansion header, plus **LED1** and the log (see below) |

BTN0 keeps its usual Silabs behaviour: a short press before commissioning opens the commissioning window and prints the QR code. LED0 (red) no longer shows the Silabs network status, because the encoder's switch uses its pin.

LED1 shows the dishwasher state:

| State | LED1 |
| ----- | ---- |
| Off | off |
| On, idle | short blip every 2 s |
| Delayed start (waiting for the energy manager) | fast blink |
| Running | solid |
| Paused | slow blink |

The same information is written to the log as `[display] RUNNING | Chef 70° | main-wash | EM opt-in | 3540s`.

### Display

A 1.3" 128x64 SH1106 I2C OLED (the common 4-pin VCC/GND/SCL/SDA module; 0.96" SSD1306 modules work too, see below) connects to the expansion header. The I2C lines are the header's standard I2C positions, which are shared with the Qwiic connector:

| OLED | xG24 Dev Kit |
| ---- | ------------ |
| VCC | 3V3 |
| GND | GND |
| SCL | EXP15 (PC04) |
| SDA | EXP16 (PC05) |
| RST (if the module has one) | EXP10 (PA07), with `SSD1306_HAS_RESET 1` |

The 7-pin modules (with D0, D1, RES, DC and CS) come set for SPI. Even converted to I2C (R3 moved to R1, R8 bridged, DC and CS grounded), the one tried here never ACKed, so use a 4-pin module.

The OLED shows:
- **Normal screen:** the state in an inverted title bar, with **EM** at the right when you've opted into energy management. Below that: the program in large text, the phase, the time left while a program runs, and the price on the bottom row.
- **Delayed start:** a large countdown, with the program and the price below it.

The display is off while the dishwasher is off.

If the display doesn't respond at boot, the log shows `[oled] no ACK from display at 0x3c` and the dishwasher carries on without it. The driver settings live in `include/Ssd1306.h`, and each can be overridden with a define:

| Define | Default | Use |
| ------ | ------- | --- |
| `SSD1306_I2C_ADDRESS` | `0x3C` | `0x3D` for modules with the address jumper moved |
| `SSD1306_HAS_RESET` | `0` | `1` for modules with a RST pin |
| `SSD1306_RESET_PORT` / `SSD1306_RESET_PIN` | `SL_GPIO_PORT_A` / `7` | The RST pin |
| `SSD1306_ROTATE_180` | `1` | `0` if the image is upside down |
| `SSD1306_SH1106` | `1` | `0` for 0.96" SSD1306 modules; the 1.3" modules use an SH1106 |

The I2C pins and the 400 kHz bus speed are in `config/sl_i2c_qwiic_config.h`. The driver uses the SDK's `i2c` component rather than I2CSPM, whose transfers can block for 5 minutes with interrupts off when the bus is stuck. Most OLED breakouts have I2C pull-ups on board. If yours doesn't, add 4.7 kΩ pull-ups to 3V3. The on-board sensors share PC04/PC05 and stay unpowered. If the bus looks stuck, try powering them with `SL_BOARD_ENABLE_SENSOR_RHT 1` in `config/sl_board_control_config.h`.

### Rotary encoder

A KY-040 style encoder with a push switch. Only EXP10 is otherwise free (EXP5 and EXP7 aren't connected), so two of its lines share pins with the RGB LED:

| Encoder | xG24 Dev Kit | Shared with |
| ------- | ------------ | ----------- |
| CLK | EXP10 (PA07) | — |
| DT | EXP9 (PB00) | Blue LED (unused); it lights as the knob turns |
| SW | EXP13 (PD02) | Red LED, formerly the network status LED; it lights while the knob is pressed |
| + | 3V3 | |
| GND | GND | |

With the dishwasher on and no program started, turning the knob moves through Eco, Chef, Quick, Heavy and **Settings**. Settings isn't a program: clicking it opens the **SETTINGS** page, a list of three items. Turn to choose, and click to open:

- **Commissioning** opens the commissioning window, so you can add the dishwasher to another Matter controller (multi-admin). The page shows the QR code and the 11-digit manual pairing code. A click closes the window and goes back. The page also goes back by itself when the window closes, after 15 minutes or once a controller has finished commissioning.
- **Energy Mgmt** shows Opt In and Opt Out, starting on the current choice. Turn to choose, and click to apply it and go back. The Settings page shows the current choice (In/Out) next to the item.
- **Exit** returns to the main screen.

On the Commissioning and Energy Mgmt pages, holding the knob goes back to Settings without changing anything. Settings itself is left with Exit.

Clicking a program starts it:

- With no price or grid-conditions forecast, it starts straight away.
- Otherwise a **START** menu offers **Now**, plus **Cheapest** when there's a price forecast and **Greenest** when there's a grid-conditions forecast. Each shows how long it would wait (`+1:30:00`). Turn to choose, click to start, or hold to go back.
- **Now** starts immediately, and the DEM forecast has no start window, so an energy manager can't move it.
- **Cheapest** and **Greenest** pick the start, within the next 24 hours, that makes the whole program cheapest or lowest in carbon intensity, from the current value and the forecast. The display counts down to it. If you're opted in, an energy manager can still move it.

While the program waits to start, a click does nothing. While it runs, a click pauses or resumes it. Holding the knob for a second cancels the program at any point.

The pins, the steps per click (`ROTARY_ENCODER_STEPS_PER_DETENT`, 4 by default; some encoders need 2), the long-press time (`ROTARY_ENCODER_LONG_PRESS_MS`, 1000) and the direction (`ROTARY_ENCODER_REVERSE`) are defines in `include/RotaryEncoder.h`.

### Shell

Connect to the VCOM serial port at 115200 baud. The shell runs on the same console.

```
dishwasher power          # toggle On/Off
dishwasher start          # start / pause / resume
dishwasher cancel
dishwasher mode next|0|1|2|3
dishwasher optin on|off
dishwasher status
dishwasher price          # current and forecast electricity price
dishwasher grid           # current and forecast grid carbon intensity
```

## Building

You need Simplicity Studio 6, or the `slc` CLI, with Simplicity SDK 2026.6.0 and the Matter extension 2.9.0 installed.

```
SLC=~/.silabs/slt/installs/archive/slc-cli-v6.0.25/slc_cli/slc
SDK=~/.silabs/slt/installs/conan/p/simpl508ee6c1a6569/p      # Simplicity SDK 2026.6.0
MAT=~/.silabs/slt/installs/conan/p/matte295087ec9511f/p      # Matter extension 2.9.0

$SLC generate --sdk-package-path=$SDK,$MAT -p matter_mg24_acme_dishwasher.slcp -o cmake -cp
cd cmake_gcc && cmake --workflow --preset project
```

The image is written to `cmake_gcc/build/base/matter_mg24_acme_dishwasher.s37`, and a copy goes to `artifact/`.

## Flashing

The application needs a bootloader. Build and flash the `matter_bootloader_external_series_2` project once, which you already have in your Studio workspace. Then flash the app:

```
commander flash cmake_gcc/build/base/matter_mg24_acme_dishwasher.s37
```

## Commissioning

The device uses the test setup code `20202021` and discriminator `3840` (`0xF00`). Press BTN0 to start BLE advertising, then:

```
chip-tool pairing ble-thread 0x05 hex:<thread dataset> 20202021 3840
```

## Time

The energy forecast needs wall-clock time. The ESP32 got it from SNTP, but there is no SNTP over Thread, and the Silabs platform does not implement `GetClock_RealTime()`. Instead the device exposes the Time Synchronization cluster. Commissioners that support it (chip-tool does) set the time during commissioning, or you can set it by hand:

```
chip-tool timesynchronization set-utctime <matter-epoch-us> 3 0x05 0
```

The time is only held in RAM, so after a reboot you need to set it again. Until the time is set, programs still run but no forecast is published (you'll see `Time not synchronised` in the log).

**Note:** forecast and start-time values are in **Matter epoch** seconds (since 2000-01-01), as the spec requires. The ESP32 version used Unix epoch seconds.

## Using

```
chip-tool onoff on 0x05 0x01
chip-tool dishwashermode read supported-modes 0x05 0x01
chip-tool dishwashermode change-to-mode 1 0x05 0x01
chip-tool operationalstate start 0x05 0x01
chip-tool deviceenergymanagement read forecast 0x05 0x02
chip-tool deviceenergymanagement start-time-adjust-request <matter-epoch-s> 1 0x05 0x02   # cause 1 = grid optimisation
```

## Device Energy Management

Starting a program publishes a forecast with one slot per program step. The programs are shaped like real ones: the 2 kW heater only runs while it heats water for the main wash and the final rinse, and the pump draws 0.1–0.2 kW the rest of the time. Lengths and energy match published figures for each kind of program:

| Program | Length | Energy | Steps (minutes at kW) |
| ------- | ------ | ------ | --------------------- |
| Eco 50° | 3 h 30 | 0.91 kWh | pre-soak 15 at 0.1, **heat 12 at 2**, wash 60 at 0.15, rinse 30 at 0.1, **hot rinse 8 at 2**, dry 85 at 0.01 |
| Chef 70° | 2 h 15 | 1.67 kWh | pre-soak 10 at 0.1, **heat 18 at 2**, wash 40 at 0.2, rinse 15 at 0.1, **hot rinse 20 at 2**, hold 10 at 0.5, heated dry 22 at 0.4 |
| Quick 45° | 0 h 30 | 0.53 kWh | **heat 9 at 2**, wash 12 at 0.15, **hot rinse 6 at 2**, drain 3 at 0.05 |
| Heavy 65° | 2 h 00 | 1.34 kWh | **heated pre-wash 8 at 2**, **heat 15 at 2**, wash 35 at 0.2, rinse 15 at 0.1, **hot rinse 12 at 2**, dry 35 at 0.05 |

The Operational State phase (pre-soak, main-wash, rinse, final-rinse, drying) follows the steps. The programs are defined in `kPrograms` in `src/DishwasherManager.cpp`. Cheapest and Greenest weight each step by its power, so they try to put the heating steps in the cheapest or greenest periods.

When the program is started with the Matter Start command or `dishwasher start` and you have opted in, the start is delayed by 60 s so an energy manager can move it with `StartTimeAdjustRequest`. The same applies to a Cheapest or Greenest start. The forecast's earliest start is now and its latest end is 24 h away. A program started with **Now** on the encoder has no start window, and adjustments are refused.

## Commodity Price

The dishwasher can read the current electricity price, and the price forecast, from a Commodity Price server such as an Electrical Energy Tariff device ([matter-esp32-electrical-energy-tariff-device](../matter-esp32-electrical-energy-tariff-device)). The price appears on the OLED and the `[display]` line, and `dishwasher price` logs the details. When the server has a price forecast, the encoder's **Cheapest** start uses it.

Matter devices can't discover each other, so you tell the dishwasher where the tariff device is with a **Binding**. The tariff device also needs an **ACL** entry that lets the dishwasher read it. With the dishwasher as node `0x05` and the tariff device as node `0x10`, with prices on endpoint 1, both on chip-tool's fabric:

```
# On the tariff device: keep chip-tool as admin, and give the dishwasher View access to Commodity Price (149)
chip-tool accesscontrol write acl '[{"fabricIndex":1,"privilege":5,"authMode":2,"subjects":[112233],"targets":null},{"fabricIndex":1,"privilege":1,"authMode":2,"subjects":[5],"targets":[{"cluster":149,"endpoint":1,"deviceType":null}]}]' 0x10 0

# On the dishwasher: bind endpoint 1's Commodity Price client to the tariff device
chip-tool binding write binding '[{"node":16,"endpoint":1,"cluster":149}]' 0x05 1
```

The dishwasher then subscribes to TariffUnit, Currency, CurrentPrice and PriceForecast. If the tariff device restarts, the dishwasher resubscribes automatically. Writing an empty binding list (`'[]'`) stops the subscription.

`PriceForecast` only exists on servers with the **Forecasting (FORE)** feature. The ESP32 tariff device doesn't have it yet, so `dishwasher price` reports "Forecast not supported by server". The SDK's Linux `energy-gateway-app` has a Commodity Price server with forecasts, if you want to test with one.

## Electrical Grid Conditions

The **Greenest** start uses the grid's carbon intensity (gCO2/kWh) from an Electrical Grid Conditions server, found the same way: a binding for cluster 160, and an ACL entry on that server giving the dishwasher View access to cluster 160. A binding write replaces the whole table, so list both bindings together. For example, with the grid conditions on node `0x11`, endpoint 1:

```
chip-tool binding write binding '[{"node":16,"endpoint":1,"cluster":149},{"node":17,"endpoint":1,"cluster":160}]' 0x05 1
```

The dishwasher subscribes to CurrentConditions and ForecastConditions. Greenest is only offered when the server has the **Forecasting (FORE)** feature. `dishwasher grid` logs what it has received.

Reading another device needs the Matter read client (`matter_read_client`), which the Silabs build leaves out by default; this project enables it.

## Differences from the ESP32 version

- **Threading.** All dishwasher logic runs on the Matter thread. The 1-second program tick is a CHIP system timer rather than a FreeRTOS task, and buttons and shell commands are passed over with `ScheduleWork`. This removes the mix of `ScheduleWork` and `LockChipStack`.
- **Dead Front.** Start, Pause and Resume are rejected with `CommandInvalidInState` while the dishwasher is off, which closes the README's "I don't ignore commands when the device is off" gap. Dishwasher Mode changes are rejected while a program is running.
- **Mode on stop.** Stopping resets the program to Eco, as before, but now also updates the Dishwasher Mode cluster's `CurrentMode`. Before, only the internal state was reset.
- **Program end.** Finishing a program emits the Operational State `OperationCompletion` event (it was a TODO).
- **DEM.** The SDK's reference DEM delegate now owns the Forecast and OptOutState attributes. The forecast ID increments on each new forecast, and the forecast is cleared to `null` (rather than zeroed) when the program stops. AbsMaxPower is 2 kW, the heater (it was 1 W). Start-time adjustment is refused when you are not opted in, the program has already started, or the time is unknown.
- **CountdownTime** is `null` when no program is selected, and includes the delayed-start time.
- **Display.** The ESP32 drew the OLED with LVGL. Here a small built-in SSD1306 driver (`src/Ssd1306.cpp`) draws it, with a 5x7 font, and only sends the parts of the screen that change. There is no menu or reset screen, because those are shell commands and BTN0 here.
- **Programs.** Each program has a realistic power profile, and the phases follow it. On the ESP32, every slot was 3 kW and the phases changed only in the last 20 seconds. There's also a fourth program, Heavy 65°.
