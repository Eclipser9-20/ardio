// LiquidCrystal_I2C.h — ardio's own driver declaration for HD44780-compatible
// character LCDs behind a PCF8574 I2C backpack.
//
// API-compatible with the widely used LiquidCrystal_I2C sketch interface. The
// nibble-mode command sequencing and the character timing live in ardio's
// assembly runtime (runtime/lcd.S), which talks to the backpack through the
// same TWI hardware as Wire.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_LIQUIDCRYSTAL_I2C_H
#define ARDIO_LIQUIDCRYSTAL_I2C_H

#include "Arduino.h"

extern "C" {
void lcd_init(uint8_t address, uint8_t cols, uint8_t rows);
void lcd_clear(void);
void lcd_home(void);
void lcd_set_cursor(uint8_t col, uint8_t row);
void lcd_write_char(uint8_t value);
void lcd_write_str(const char *text);
void lcd_command(uint8_t value);
void lcd_backlight(uint8_t on);
void lcd_display(uint8_t on);
void lcd_cursor(uint8_t on);
void lcd_blink(uint8_t on);
void lcd_create_char(uint8_t slot, const uint8_t *rows);
}

class LiquidCrystal_I2C {
public:
    // addr is the backpack's 7-bit I2C address (commonly 0x27 or 0x3F); cols
    // and rows are the panel geometry, e.g. (16, 2) or (20, 4).
    LiquidCrystal_I2C(uint8_t addr, uint8_t cols, uint8_t rows);

    // init() and begin() are the same operation under two names, because
    // sketches in the wild call either. Both power up the panel in 4-bit mode,
    // clear it, and leave the cursor at (0, 0).
    void init();
    void begin();

    void backlight();
    void noBacklight();

    void clear();
    void home();

    // Column 0..cols-1, row 0..rows-1.
    void setCursor(uint8_t col, uint8_t row);

    void print(const char *text);
    void print(int value);
    void print(long value);
    void print(char c);
    void print(const String &text);

    // One character (or one custom-glyph slot number) at the cursor.
    void write(uint8_t value);

    void display();
    void noDisplay();
    void cursor();
    void noCursor();
    void blink();
    void noBlink();

    // Define one of the eight custom glyphs, from 8 rows of 5 bits each.
    void createChar(uint8_t slot, const uint8_t *rows);

private:
    uint8_t address_;
    uint8_t cols_;
    uint8_t rows_;
    uint8_t backlight_;
};

#endif // ARDIO_LIQUIDCRYSTAL_I2C_H
