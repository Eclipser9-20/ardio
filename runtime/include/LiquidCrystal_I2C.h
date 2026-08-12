// LiquidCrystal_I2C.h — ardio's own driver declaration for HD44780-compatible
// character LCDs behind a PCF8574 I2C backpack.
//
// API-compatible with the widely used LiquidCrystal_I2C sketch interface. The
// nibble-mode command sequencing and the character timing live in ardio's
// assembly runtime (runtime/lcd.S), which talks to the backpack through the
// same TWI hardware as Wire.
//
// Written inside the C++ subset ardio's own compiler accepts; see Arduino.h for
// the full list. Here: no `extern "C"`, no typedefs, and print() is split by
// argument type — print_str/print_int/print_long/print_char — because the
// compiler keys functions by name alone, so the four published overloads would
// collide and only one would survive. The String-taking overload is dropped
// entirely: there is no String type in the subset.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_LIQUIDCRYSTAL_I2C_H
#define ARDIO_LIQUIDCRYSTAL_I2C_H

// Runtime entry points, exported by runtime/lcd.S as plain labels.
void lcd_init(int address, int cols, int rows);
void lcd_clear();
void lcd_home();
void lcd_set_cursor(int col, int row);
void lcd_write_char(int value);
void lcd_write_str(const char *text);
void lcd_command(int value);
void lcd_backlight(int on);
void lcd_display(int on);
void lcd_cursor(int on);
void lcd_blink(int on);
void lcd_create_char(int slot, const char *rows);

class LiquidCrystal_I2C {
public:
    // addr is the backpack's 7-bit I2C address (commonly 0x27 or 0x3F); cols
    // and rows are the panel geometry, e.g. (16, 2) or (20, 4).
    LiquidCrystal_I2C(int addr, int cols, int rows);

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
    void setCursor(int col, int row);

    // Divergence: one print() per argument type, since names must be unique.
    void print_str(const char *text);
    void print_int(int value);
    void print_long(long value);
    void print_char(char c);

    // One character (or one custom-glyph slot number) at the cursor.
    void write(int value);

    void display();
    void noDisplay();
    void cursor();
    void noCursor();
    void blink();
    void noBlink();

    // Define one of the eight custom glyphs, from 8 rows of 5 bits each.
    void createChar(int slot, const char *rows);

private:
    int address_;
    int cols_;
    int rows_;
    int backlight_;
};

#endif // ARDIO_LIQUIDCRYSTAL_I2C_H
