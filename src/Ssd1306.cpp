#include "Ssd1306.h"

#include "sl_gpio.h"
#include "sl_sleeptimer.h"

#include <lib/support/logging/CHIPLogging.h>

#include <string.h>

namespace {

constexpr uint8_t kControlCommand = 0x00;
constexpr uint8_t kControlData    = 0x40;
constexpr uint8_t kPages          = Ssd1306::kHeight / 8;

// A page is 129 bytes, about 3 ms at 400 kHz. Transfers run with interrupts off, so a stuck bus
// must give up quickly.
constexpr uint32_t kTransferTimeoutMs = 20;

#if SSD1306_SH1106
constexpr uint8_t kColumnOffset = 2;
#endif

// clang-format off
const uint8_t kInitCommands[] = {
    0xAE,       // display off
    0xD5, 0x80, // clock divide ratio / oscillator frequency
    0xA8, 0x3F, // multiplex ratio: 64 rows
    0xD3, 0x00, // display offset
    0x40,       // start line 0
    0x8D, 0x14, // charge pump on
    0x20, 0x00, // horizontal addressing (ignored by the SH1106)
#if SSD1306_ROTATE_180
    0xA1,       // segment remap: column 127 -> SEG0
    0xC8,       // COM scan from COM63 to COM0
#else
    0xA0,
    0xC0,
#endif
    0xDA, 0x12, // COM pins: alternative, no remap
    0x81, 0xCF, // contrast
    0xD9, 0xF1, // pre-charge period
    0xDB, 0x40, // VCOMH deselect level
    0xA4,       // display follows RAM
    0xA6,       // not inverted
};

// 5x7 ASCII font, 0x20 to 0x7E. Each byte is one column, LSB at the top.
const uint8_t kFont5x7[][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, // ' '
    { 0x00, 0x00, 0x5F, 0x00, 0x00 }, // !
    { 0x00, 0x07, 0x00, 0x07, 0x00 }, // "
    { 0x14, 0x7F, 0x14, 0x7F, 0x14 }, // #
    { 0x24, 0x2A, 0x7F, 0x2A, 0x12 }, // $
    { 0x23, 0x13, 0x08, 0x64, 0x62 }, // %
    { 0x36, 0x49, 0x56, 0x20, 0x50 }, // &
    { 0x00, 0x00, 0x07, 0x00, 0x00 }, // '
    { 0x00, 0x1C, 0x22, 0x41, 0x00 }, // (
    { 0x00, 0x41, 0x22, 0x1C, 0x00 }, // )
    { 0x2A, 0x1C, 0x7F, 0x1C, 0x2A }, // *
    { 0x08, 0x08, 0x3E, 0x08, 0x08 }, // +
    { 0x00, 0x50, 0x30, 0x00, 0x00 }, // ,
    { 0x08, 0x08, 0x08, 0x08, 0x08 }, // -
    { 0x00, 0x60, 0x60, 0x00, 0x00 }, // .
    { 0x20, 0x10, 0x08, 0x04, 0x02 }, // /
    { 0x3E, 0x51, 0x49, 0x45, 0x3E }, // 0
    { 0x00, 0x42, 0x7F, 0x40, 0x00 }, // 1
    { 0x42, 0x61, 0x51, 0x49, 0x46 }, // 2
    { 0x21, 0x41, 0x45, 0x4B, 0x31 }, // 3
    { 0x18, 0x14, 0x12, 0x7F, 0x10 }, // 4
    { 0x27, 0x45, 0x45, 0x45, 0x39 }, // 5
    { 0x3C, 0x4A, 0x49, 0x49, 0x30 }, // 6
    { 0x01, 0x71, 0x09, 0x05, 0x03 }, // 7
    { 0x36, 0x49, 0x49, 0x49, 0x36 }, // 8
    { 0x06, 0x49, 0x49, 0x29, 0x1E }, // 9
    { 0x00, 0x36, 0x36, 0x00, 0x00 }, // :
    { 0x00, 0x56, 0x36, 0x00, 0x00 }, // ;
    { 0x08, 0x14, 0x22, 0x41, 0x00 }, // <
    { 0x14, 0x14, 0x14, 0x14, 0x14 }, // =
    { 0x00, 0x41, 0x22, 0x14, 0x08 }, // >
    { 0x02, 0x01, 0x51, 0x09, 0x06 }, // ?
    { 0x32, 0x49, 0x79, 0x41, 0x3E }, // @
    { 0x7E, 0x11, 0x11, 0x11, 0x7E }, // A
    { 0x7F, 0x49, 0x49, 0x49, 0x36 }, // B
    { 0x3E, 0x41, 0x41, 0x41, 0x22 }, // C
    { 0x7F, 0x41, 0x41, 0x22, 0x1C }, // D
    { 0x7F, 0x49, 0x49, 0x49, 0x41 }, // E
    { 0x7F, 0x09, 0x09, 0x09, 0x01 }, // F
    { 0x3E, 0x41, 0x49, 0x49, 0x7A }, // G
    { 0x7F, 0x08, 0x08, 0x08, 0x7F }, // H
    { 0x00, 0x41, 0x7F, 0x41, 0x00 }, // I
    { 0x20, 0x40, 0x41, 0x3F, 0x01 }, // J
    { 0x7F, 0x08, 0x14, 0x22, 0x41 }, // K
    { 0x7F, 0x40, 0x40, 0x40, 0x40 }, // L
    { 0x7F, 0x02, 0x0C, 0x02, 0x7F }, // M
    { 0x7F, 0x04, 0x08, 0x10, 0x7F }, // N
    { 0x3E, 0x41, 0x41, 0x41, 0x3E }, // O
    { 0x7F, 0x09, 0x09, 0x09, 0x06 }, // P
    { 0x3E, 0x41, 0x51, 0x21, 0x5E }, // Q
    { 0x7F, 0x09, 0x19, 0x29, 0x46 }, // R
    { 0x46, 0x49, 0x49, 0x49, 0x31 }, // S
    { 0x01, 0x01, 0x7F, 0x01, 0x01 }, // T
    { 0x3F, 0x40, 0x40, 0x40, 0x3F }, // U
    { 0x1F, 0x20, 0x40, 0x20, 0x1F }, // V
    { 0x3F, 0x40, 0x38, 0x40, 0x3F }, // W
    { 0x63, 0x14, 0x08, 0x14, 0x63 }, // X
    { 0x07, 0x08, 0x70, 0x08, 0x07 }, // Y
    { 0x61, 0x51, 0x49, 0x45, 0x43 }, // Z
    { 0x00, 0x7F, 0x41, 0x41, 0x00 }, // [
    { 0x02, 0x04, 0x08, 0x10, 0x20 }, // backslash
    { 0x00, 0x41, 0x41, 0x7F, 0x00 }, // ]
    { 0x04, 0x02, 0x01, 0x02, 0x04 }, // ^
    { 0x40, 0x40, 0x40, 0x40, 0x40 }, // _
    { 0x00, 0x01, 0x02, 0x04, 0x00 }, // `
    { 0x20, 0x54, 0x54, 0x54, 0x78 }, // a
    { 0x7F, 0x48, 0x44, 0x44, 0x38 }, // b
    { 0x38, 0x44, 0x44, 0x44, 0x20 }, // c
    { 0x38, 0x44, 0x44, 0x48, 0x7F }, // d
    { 0x38, 0x54, 0x54, 0x54, 0x18 }, // e
    { 0x08, 0x7E, 0x09, 0x01, 0x02 }, // f
    { 0x0C, 0x52, 0x52, 0x52, 0x3E }, // g
    { 0x7F, 0x08, 0x04, 0x04, 0x78 }, // h
    { 0x00, 0x44, 0x7D, 0x40, 0x00 }, // i
    { 0x20, 0x40, 0x44, 0x3D, 0x00 }, // j
    { 0x7F, 0x10, 0x28, 0x44, 0x00 }, // k
    { 0x00, 0x41, 0x7F, 0x40, 0x00 }, // l
    { 0x7C, 0x04, 0x18, 0x04, 0x78 }, // m
    { 0x7C, 0x08, 0x04, 0x04, 0x78 }, // n
    { 0x38, 0x44, 0x44, 0x44, 0x38 }, // o
    { 0x7C, 0x14, 0x14, 0x14, 0x08 }, // p
    { 0x08, 0x14, 0x14, 0x18, 0x7C }, // q
    { 0x7C, 0x08, 0x04, 0x04, 0x08 }, // r
    { 0x48, 0x54, 0x54, 0x54, 0x20 }, // s
    { 0x04, 0x3F, 0x44, 0x40, 0x20 }, // t
    { 0x3C, 0x40, 0x40, 0x20, 0x7C }, // u
    { 0x1C, 0x20, 0x40, 0x20, 0x1C }, // v
    { 0x3C, 0x40, 0x30, 0x40, 0x3C }, // w
    { 0x44, 0x28, 0x10, 0x28, 0x44 }, // x
    { 0x0C, 0x50, 0x50, 0x50, 0x3C }, // y
    { 0x44, 0x64, 0x54, 0x4C, 0x44 }, // z
    { 0x00, 0x08, 0x36, 0x41, 0x00 }, // {
    { 0x00, 0x00, 0x7F, 0x00, 0x00 }, // |
    { 0x00, 0x41, 0x36, 0x08, 0x00 }, // }
    { 0x08, 0x04, 0x08, 0x10, 0x08 }, // ~
};
// clang-format on

const uint8_t kDegreeGlyph[5]   = { 0x00, 0x06, 0x09, 0x09, 0x06 };
const uint8_t * const kUnknownGlyph = kFont5x7['?' - ' '];

// Returns the glyph for the character at text and advances past it, or nullptr at the end of the string.
// Only ASCII and the UTF-8 degree sign have glyphs; any other multi-byte character draws as '?'.
const uint8_t * NextGlyph(const char *& text)
{
    auto c = static_cast<uint8_t>(*text);
    if (c == '\0')
    {
        return nullptr;
    }
    text++;

    if (c >= ' ' && c <= '~')
    {
        return kFont5x7[c - ' '];
    }
    if (c == 0xC2 && static_cast<uint8_t>(*text) == 0xB0)
    {
        text++;
        return kDegreeGlyph;
    }
    while ((static_cast<uint8_t>(*text) & 0xC0) == 0x80)
    {
        text++;
    }
    return kUnknownGlyph;
}

} // namespace

bool Ssd1306::Init(sl_i2c_handle_t * i2c)
{
    mI2c = i2c;

#if SSD1306_HAS_RESET
    const sl_gpio_t reset = { SSD1306_RESET_PORT, SSD1306_RESET_PIN };
    // Generous timings, as some modules have an RC network on RST.
    sl_gpio_set_pin_mode(&reset, SL_GPIO_MODE_PUSH_PULL, false);
    sl_sleeptimer_delay_millisecond(10);
    sl_gpio_set_pin(&reset);
    sl_sleeptimer_delay_millisecond(10);
#endif

    if (!Write(kControlCommand, kInitCommands, sizeof(kInitCommands)))
    {
        // 0x0D (SL_STATUS_NOT_FOUND) is a NACK; 0x0B (SL_STATUS_TIMEOUT) means the bus is stuck, usually with
        // SCL held low; anything else is a bus error, which points at the wiring or pull-ups.
        ChipLogError(AppServer, "[oled] no ACK from display at 0x%02x (status 0x%04lx), running without it",
                     SSD1306_I2C_ADDRESS, static_cast<unsigned long>(mLastResult));
        ScanBus();
        return false;
    }

    mReady      = true;
    mForceFlush = true;
    Clear();
    Flush();

    ChipLogProgress(AppServer, "[oled] %s ready at 0x%02x", SSD1306_SH1106 ? "SH1106" : "SSD1306", SSD1306_I2C_ADDRESS);
    return true;
}

void Ssd1306::SetPower(bool on)
{
    if (!mReady)
    {
        return;
    }
    const uint8_t command = on ? 0xAF : 0xAE;
    Write(kControlCommand, &command, 1);
}

void Ssd1306::Clear()
{
    memset(mBuffer, 0, sizeof(mBuffer));
}

void Ssd1306::SetPixel(int x, int y, bool on)
{
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight)
    {
        return;
    }
    uint8_t & column = mBuffer[(y / 8) * kWidth + x];
    const uint8_t bit = static_cast<uint8_t>(1u << (y % 8));
    column            = on ? (column | bit) : (column & ~bit);
}

void Ssd1306::FillRect(int x, int y, int w, int h, bool on)
{
    for (int row = y; row < y + h; row++)
    {
        for (int col = x; col < x + w; col++)
        {
            SetPixel(col, row, on);
        }
    }
}

void Ssd1306::DrawGlyph(int x, int y, const uint8_t * glyph, uint8_t scale, bool on)
{
    for (int col = 0; col < 5; col++)
    {
        for (int row = 0; row < 7; row++)
        {
            if (glyph[col] & (1u << row))
            {
                FillRect(x + col * scale, y + row * scale, scale, scale, on);
            }
        }
    }
}

void Ssd1306::DrawText(int x, int y, const char * text, uint8_t scale, bool on)
{
    while (const uint8_t * glyph = NextGlyph(text))
    {
        if (x >= kWidth)
        {
            break;
        }
        DrawGlyph(x, y, glyph, scale, on);
        x += kCharWidth * scale;
    }
}

int Ssd1306::TextWidth(const char * text, uint8_t scale)
{
    int chars = 0;
    while (NextGlyph(text) != nullptr)
    {
        chars++;
    }
    // Drop the spacing column after the last character.
    return chars == 0 ? 0 : chars * kCharWidth * scale - scale;
}

void Ssd1306::Flush()
{
    if (!mReady)
    {
        return;
    }

    for (uint8_t page = 0; page < kPages; page++)
    {
        const uint8_t * pageData = &mBuffer[page * kWidth];
        uint8_t * shown          = &mShown[page * kWidth];
        if (!mForceFlush && memcmp(pageData, shown, kWidth) == 0)
        {
            continue;
        }

#if SSD1306_SH1106
        const uint8_t address[] = { static_cast<uint8_t>(0xB0 | page), static_cast<uint8_t>(kColumnOffset & 0x0F),
                                    static_cast<uint8_t>(0x10 | (kColumnOffset >> 4)) };
#else
        const uint8_t address[] = { 0x21, 0, kWidth - 1, 0x22, page, page };
#endif
        if (!Write(kControlCommand, address, sizeof(address)) || !Write(kControlData, pageData, kWidth))
        {
            // Try the whole frame again next time.
            mForceFlush = true;
            return;
        }
        memcpy(shown, pageData, kWidth);
    }

    mForceFlush = false;
}

// Logs every address that ACKs a one-byte read, to help with wiring and address problems.
void Ssd1306::ScanBus()
{
    int found = 0;
    for (uint8_t address = 0x08; address < 0x78; address++)
    {
        uint8_t byte;
        if (sl_i2c_leader_receive_blocking(mI2c, address, &byte, 1, kTransferTimeoutMs) == SL_STATUS_OK)
        {
            ChipLogProgress(AppServer, "[oled] I2C device at 0x%02x", address);
            found++;
        }
    }
    if (found == 0)
    {
        ChipLogError(AppServer, "[oled] no I2C devices found; check power, wiring and SDA/SCL");
    }
}

bool Ssd1306::Write(uint8_t control, const uint8_t * bytes, size_t length)
{
    // The control byte has to lead the same transfer, so the bytes are copied in after it.
    uint8_t buffer[1 + kWidth];
    if (length > kWidth)
    {
        mLastResult = SL_STATUS_INVALID_PARAMETER;
        return false;
    }
    buffer[0] = control;
    memcpy(&buffer[1], bytes, length);

    mLastResult = sl_i2c_leader_send_blocking(mI2c, SSD1306_I2C_ADDRESS, buffer, static_cast<uint32_t>(length + 1),
                                              kTransferTimeoutMs);
    return mLastResult == SL_STATUS_OK;
}
