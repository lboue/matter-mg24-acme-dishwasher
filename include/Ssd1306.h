#pragma once

#include "sl_i2c.h"

#include <stddef.h>
#include <stdint.h>

// 7-bit I2C address. Modules with the address jumper moved use 0x3D.
#ifndef SSD1306_I2C_ADDRESS
#define SSD1306_I2C_ADDRESS 0x3C
#endif

// 4-pin modules have no RST pin. For modules with one, define SSD1306_HAS_RESET 1 and wire it to EXP10 (PA07).
#ifndef SSD1306_HAS_RESET
#define SSD1306_HAS_RESET 0
#endif
#ifndef SSD1306_RESET_PORT
#define SSD1306_RESET_PORT SL_GPIO_PORT_A
#endif
#ifndef SSD1306_RESET_PIN
#define SSD1306_RESET_PIN 7
#endif

// 1 matches the orientation of the ESP32 build, which rotated the panel 180 degrees in LVGL.
#ifndef SSD1306_ROTATE_180
#define SSD1306_ROTATE_180 1
#endif

// 1.3" modules (like the one this is built for) use an SH1106, which has a 132 column RAM and no
// horizontal addressing mode. Define SSD1306_SH1106 0 for 0.96" SSD1306 modules.
#ifndef SSD1306_SH1106
#define SSD1306_SH1106 1
#endif

/**
 * Minimal driver for a 128x64 SSD1306 (or SH1106) OLED over I2C.
 *
 * Drawing goes into a local framebuffer; Flush() sends only the 8-pixel pages that changed since the
 * last flush. If the panel doesn't ACK during Init(), every call is a no-op so the app runs without it.
 */
class Ssd1306
{
public:
    static constexpr uint8_t kWidth  = 128;
    static constexpr uint8_t kHeight = 64;

    // Each character is 5x7 with one column of spacing, scaled up by an integer factor.
    static constexpr uint8_t kCharWidth  = 6;
    static constexpr uint8_t kCharHeight = 8;

    bool Init(sl_i2c_handle_t * i2c);
    bool IsReady() const { return mReady; }

    void SetPower(bool on);

    void Clear();
    void FillRect(int x, int y, int w, int h, bool on);
    void DrawText(int x, int y, const char * text, uint8_t scale = 1, bool on = true);
    static int TextWidth(const char * text, uint8_t scale = 1);

    void Flush();

private:
    bool Write(uint8_t control, const uint8_t * bytes, size_t length);
    void ScanBus();
    void SetPixel(int x, int y, bool on);
    void DrawGlyph(int x, int y, const uint8_t * glyph, uint8_t scale, bool on);

    sl_i2c_handle_t * mI2c = nullptr;
    bool mReady        = false;
    bool mForceFlush   = true;
    sl_status_t mLastResult = SL_STATUS_OK;

    uint8_t mBuffer[kWidth * kHeight / 8];
    // What the panel currently shows, so Flush() can skip unchanged pages.
    uint8_t mShown[kWidth * kHeight / 8];
};
