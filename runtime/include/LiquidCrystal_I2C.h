// LiquidCrystal_I2C.h — ardio's own driver for HD44780-compatible character
// LCDs behind a PCF8574 I2C backpack.
//
// API-compatible with the widely used LiquidCrystal_I2C sketch interface. The
// nibble-mode command sequencing and the character timing live in ardio's
// assembly runtime (runtime/lcd.S), which talks to the backpack through its
// own private copy of the TWI sequence.
//
// ---------------------------------------------------------------------------
// WHAT THE RUNTIME ACTUALLY PROVIDES
//
// runtime/lcd.S exports five labels and no more:
//
//     lcd_init(address)          power-up sequence, 4-bit mode, cleared,
//                                backlight on, cursor at (0, 0)
//     lcd_clear()
//     lcd_set_cursor(col, row)
//     lcd_write_char(c)
//     lcd_backlight(on)
//
// Every method below is an inline forward built from those. ardio has no
// linker and no symbol aliasing, so a method carrying only a declaration
// compiles and then fails at assembly time as "nothing implements
// 'LiquidCrystal_I2C__print'"; there are no declaration-only methods here.
//
// WHAT IS MISSING, AND WHY IT IS NOT DECLARED
//
//   * display()/noDisplay(), cursor()/noCursor(), blink()/noBlink() — these
//     are all one HD44780 display-control instruction, which needs an entry
//     point that sends an arbitrary command byte. lcd.S has none: lcd_send is
//     internal to it. Rather than declare them and fail at assembly time, they
//     are absent.
//   * createChar() — likewise: loading a custom glyph means writing CGRAM,
//     which again needs a command byte.
//   * scrollDisplayLeft/Right, autoscroll, leftToRight and the rest of the
//     display-control family, for the same reason.
//
// runtime/README.md carries the same list.
//
// Divergences from the published API:
//
//   * print() is overloaded on const char*, int and char, but not on long:
//     this compiler passes method arguments in registers and supports 8- and
//     16-bit ones only. The free function lcd_print_long() below covers the
//     32-bit case.
//   * The String-taking overload is not here: including WString.h would be
//     required, and its String is a view over caller-supplied storage rather
//     than the Arduino one. Print s.c_str() instead.
//   * The runtime's own row handling is two-line: odd rows go to the second
//     line's DDRAM base. On a 20x4 panel rows 2 and 3 land on rows 0 and 1.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_LIQUIDCRYSTAL_I2C_H
#define ARDIO_LIQUIDCRYSTAL_I2C_H

// Runtime entry points, exported by runtime/lcd.S as plain labels.
void lcd_init(int address);
void lcd_clear();
void lcd_set_cursor(int col, int row);
void lcd_write_char(int value);
void lcd_backlight(int on);

// Longest 32-bit decimal is "-2147483648": eleven characters plus a
// terminator's worth of headroom.
const int LCD_LONG_DIGITS = 12;

// Print a 32-bit value at the cursor. A free function rather than a method,
// because a method cannot take a 32-bit argument here.
inline void lcd_print_long(long value) {
    char digits[LCD_LONG_DIGITS];
    if (value == 0) {
        lcd_write_char('0');
        return;
    }
    bool negative = value < 0;
    // Negated as a long so that -2147483648 does not overflow on the way.
    if (negative) value = -value;
    int count = 0;
    while (value > 0 && count < LCD_LONG_DIGITS) {
        digits[count] = (char)('0' + (int)(value % 10));
        value = value / 10;
        count = count + 1;
    }
    if (negative) lcd_write_char('-');
    while (count > 0) {
        count = count - 1;
        lcd_write_char((int)digits[count]);
    }
}

class LiquidCrystal_I2C {
public:
    // addr is the backpack's 7-bit I2C address (commonly 0x27 or 0x3F); cols
    // and rows are the panel geometry, e.g. (16, 2) or (20, 4). Nothing is
    // sent to the panel until init() or begin().
    LiquidCrystal_I2C(int addr, int cols, int rows) {
        address_ = addr;
        cols_ = cols;
        rows_ = rows;
        backlight_ = 1;
    }

    // init() and begin() are the same operation under two names, because
    // sketches in the wild call either. Both power up the panel in 4-bit mode,
    // clear it, turn the backlight on, and leave the cursor at (0, 0).
    void init() {
        backlight_ = 1;
        lcd_init(address_);
    }
    void begin() { init(); }

    void backlight() {
        backlight_ = 1;
        lcd_backlight(1);
    }
    void noBacklight() {
        backlight_ = 0;
        lcd_backlight(0);
    }

    void clear() { lcd_clear(); }
    void home() { lcd_set_cursor(0, 0); }

    // Column 0..cols-1, row 0..rows-1.
    void setCursor(int col, int row) { lcd_set_cursor(col, row); }

    void print(const char *text) {
        int i = 0;
        while (text[i] != 0) {
            lcd_write_char((int)text[i]);
            i = i + 1;
        }
    }

    void print(int value) { lcd_print_long((long)value); }

    void print(char c) { lcd_write_char((int)c); }

    // One character (or one custom-glyph slot number) at the cursor.
    void write(int value) { lcd_write_char(value); }

    // The geometry given to the constructor.
    int columns() { return cols_; }
    int rows() { return rows_; }

    // The backlight state this object last asked for.
    bool backlightOn() { return backlight_ != 0; }

private:
    int address_;
    int cols_;
    int rows_;
    int backlight_;
};

#endif // ARDIO_LIQUIDCRYSTAL_I2C_H
