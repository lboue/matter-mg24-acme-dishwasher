#pragma once

#include <stdint.h>

// CLK on EXP10 (PA07), which is otherwise free.
#ifndef ROTARY_ENCODER_CLK_PORT
#define ROTARY_ENCODER_CLK_PORT SL_GPIO_PORT_A
#endif
#ifndef ROTARY_ENCODER_CLK_PIN
#define ROTARY_ENCODER_CLK_PIN 7
#endif

// DT on EXP9 (PB00), shared with the blue LED, which the app doesn't use. The blue LED lights
// while this contact is closed.
#ifndef ROTARY_ENCODER_DT_PORT
#define ROTARY_ENCODER_DT_PORT SL_GPIO_PORT_B
#endif
#ifndef ROTARY_ENCODER_DT_PIN
#define ROTARY_ENCODER_DT_PIN 0
#endif

// SW on EXP13 (PD02), shared with the red LED. That LED was the Silabs network status LED, which
// RotaryEncoder::Init() disconnects. The red LED lights while the knob is pressed.
#ifndef ROTARY_ENCODER_SW_PORT
#define ROTARY_ENCODER_SW_PORT SL_GPIO_PORT_D
#endif
#ifndef ROTARY_ENCODER_SW_PIN
#define ROTARY_ENCODER_SW_PIN 2
#endif

// Quadrature steps per click of the knob. KY-040 style encoders do a full cycle (4) per detent;
// some do half a cycle (2).
#ifndef ROTARY_ENCODER_STEPS_PER_DETENT
#define ROTARY_ENCODER_STEPS_PER_DETENT 4
#endif

// How long the knob must be held for a long press.
#ifndef ROTARY_ENCODER_LONG_PRESS_MS
#define ROTARY_ENCODER_LONG_PRESS_MS 1000
#endif

// Swaps the turn direction, if clockwise selects the previous program.
#ifndef ROTARY_ENCODER_REVERSE
#define ROTARY_ENCODER_REVERSE 0
#endif

/**
 * Rotary encoder with push switch, read with GPIO interrupts.
 *
 * All callbacks run in interrupt context, so they must only hand the event on (e.g. post it to
 * the app task).
 */
class RotaryEncoder
{
public:
    // direction is +1 for clockwise and -1 for counter-clockwise, one call per detent.
    using TurnCallback  = void (*)(int8_t direction);
    using ClickCallback = void (*)();

    // A press shorter than ROTARY_ENCODER_LONG_PRESS_MS is a click, reported on release. Holding
    // it that long reports a long press instead, as soon as the time is up.
    static void Init(TurnCallback onTurn, ClickCallback onClick, ClickCallback onLongPress);
};
