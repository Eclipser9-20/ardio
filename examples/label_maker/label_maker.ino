// Label Maker -- a tape label printer for the Arduino Nano, compiled by ardio.
//
//     ardio build examples/label_maker/label_maker.ino
//
// The machine: a 16x2 I2C character LCD, a five-way control, two 28BYJ-48
// steppers driving an X carriage and a Y lead screw, and a servo that lifts
// the pen off the tape. A four-state machine walks you from a main menu
// through character-by-character editing and a print confirmation into
// plotting the label, one stroke-font glyph at a time, with Bresenham lines.
//
// The runtime entry points are declared here rather than pulled in from a
// header, because ardio's compiler does not process #include yet. Their
// definitions come from ardio's own assembly runtime, which is appended to
// this sketch's generated assembly at build time.
//
// README.md next to this file lists what had to change relative to an
// ordinary Arduino sketch, and why.

void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);
void delay(int ms);

void i2c_init();
void lcd_init(int address, int cols, int rows);
void lcd_clear();
void lcd_set_cursor(int col, int row);
void lcd_write_char(int c);
void lcd_backlight(int on);

void servo_attach(int pin);
void servo_write(int angle);

int button_init(int pin, int debounce_ms);
int button_pressed(int pin);

const int OUTPUT_MODE = 1;

// ------------------------------------------------------------ hardware -----

const int X_PIN1 = 6;           // X carriage motor coils
const int X_PIN2 = 8;
const int X_PIN3 = 7;
const int X_PIN4 = 9;

const int Y_PIN1 = 2;           // Y lead screw motor coils
const int Y_PIN2 = 4;
const int Y_PIN3 = 3;
const int Y_PIN4 = 5;

const int SERVO_PIN = 13;

// The joystick of the original, as five contacts to ground: there is no
// analogRead in the runtime to read a real one with.
const int BTN_OK    = 14;       // A0, the click
const int BTN_UP    = 15;       // A1
const int BTN_DOWN  = 16;       // A2
const int BTN_LEFT  = 17;       // A3
const int BTN_RIGHT = 10;
const int DEBOUNCE_MS = 50;

const int LCD_ADDR = 39;        // 0x27, the usual PCF8574 backpack address
const int PEN_UP_ANGLE   = 25;
const int PEN_DOWN_ANGLE = 80;

// Step timing. The original asks the Stepper library for 10 and 12 rpm; at
// 2048 steps per revolution that is about 2.9 ms and 2.4 ms per step, and this
// sketch drives the coils itself, so the delay is written out directly.
const int X_STEP_MS = 3;
const int Y_STEP_MS = 2;

// ------------------------------------------------------------- geometry -----

const int x_scale = 230;        // motor steps per font coordinate unit
const int y_scale = 230;
const int space = 1150;         // x_scale * 5: one character cell, in steps

int xpos = 0;                   // where the carriage is now, in steps
int ypos = 0;
int angle = 25;                 // PEN_UP_ANGLE; only literals initialise globals
bool pen_on_paper = false;

int x_phase = 0;                // full-drive phase index of each motor
int y_phase = 0;

// ---------------------------------------------------------- menu state -----

const int MAIN_MENU = 0;
const int EDITING = 1;
const int PRINT_CONFIRM = 2;
const int PRINTING = 3;

int state = 0;                  // MAIN_MENU
int prev_state = 3;             // PRINTING, so the first pass paints the menu
int cursor_position = 0;
int current_character = 0;
int blink_tick = 0;
bool blink_on = false;

// ---------------------------------------------------------- label text -----
//
// The String of the original. A fixed buffer instead: there is no heap, and
// the display is only 16 columns wide in any case.

const int TEXT_MAX = 16;
char text[17];
int text_len = 0;

void text_clear() {
    for (int i = 0; i <= TEXT_MAX; i++) text[i] = 32;
    text_len = 0;
}

void text_append(int c) {
    if (text_len >= TEXT_MAX) return;
    text[text_len] = c;
    text_len = text_len + 1;
}

void text_backspace() {
    if (text_len <= 0) return;
    text_len = text_len - 1;
    text[text_len] = 32;
}

// ------------------------------------------------------------- screen -----

void print_str(const char* s) {
    while (*s) {
        lcd_write_char(*s);
        s = s + 1;
    }
}

void print_text() {
    for (int i = 0; i < text_len; i++) lcd_write_char(text[i]);
}

// Clears the top row and reprints the label, so that deleted characters do
// not hang around at the end of the line.
void show_text() {
    lcd_set_cursor(0, 0);
    print_str(":               ");
    lcd_set_cursor(1, 0);
    print_text();
}

void reset_screen() {
    lcd_clear();
    lcd_set_cursor(0, 0);
    print_str(": ");
    lcd_set_cursor(1, 0);
    cursor_position = 1;
}

// --------------------------------------------------- the editing menu -----
//
// Slot 0 is a space, then A..Z, then 0..9, then four marks. Written as
// arithmetic rather than a lookup table because every comparison in a chain
// this long costs about twenty-five instructions of flash.

const int ALPHABET_SIZE = 41;

int alphabet(int i) {
    if (i <= 0) return 32;              // space
    if (i < 27) return i + 64;          // 'A' .. 'Z'
    if (i < 37) return i + 21;          // '0' .. '9'
    if (i == 37) return 45;             // '-'
    if (i == 38) return 46;             // '.'
    if (i == 39) return 33;             // '!'
    return 63;                          // '?'
}

// ----------------------------------------------------------- the font -----
//
// Each entry encodes one move of the pen: the hundreds digit means draw while
// moving, the tens digit is x and the ones digit is y, both on the 0..4 grid
// that the scale factors multiply up into motor steps. 200 ends a glyph and
// 222 stamps a single dot.
//
// It is a pile of functions rather than the obvious const array because ardio
// has no aggregate initialisers yet -- an array can be declared and indexed,
// but there is no way to give it its contents at compile time, and filling a
// 40x14 table at run time would eat more than half of this part's SRAM.

const int GLYPH_END = 200;
const int GLYPH_DOT = 222;

// ASCII to glyph slot, or -1 for "nothing to draw". The slots run A..Z, 0..9,
// '-', '.', '!', '?', so the two big ranges are subtractions.
int glyph_slot(int c) {
    if (c > 96 && c < 123) c = c - 32;  // fold lower case up
    if (c > 64 && c < 91) return c - 65;
    if (c > 47 && c < 58) return c - 22;
    if (c == 45) return 36;
    if (c == 46) return 37;
    if (c == 33) return 38;
    if (c == 63) return 39;
    return -1;
}

int glyph_0(int i) {   // 'A'
    if (i == 0) return 0;
    if (i == 1) return 103;
    if (i == 2) return 114;
    if (i == 3) return 134;
    if (i == 4) return 143;
    if (i == 5) return 140;
    if (i == 6) return 2;
    if (i == 7) return 142;
    return GLYPH_END;
}

int glyph_1(int i) {   // 'B'
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 134;
    if (i == 3) return 143;
    if (i == 4) return 132;
    if (i == 5) return 102;
    if (i == 6) return 32;
    if (i == 7) return 141;
    if (i == 8) return 130;
    if (i == 9) return 100;
    return GLYPH_END;
}

int glyph_2(int i) {   // 'C'
    if (i == 0) return 44;
    if (i == 1) return 114;
    if (i == 2) return 103;
    if (i == 3) return 101;
    if (i == 4) return 110;
    if (i == 5) return 140;
    return GLYPH_END;
}

int glyph_3(int i) {   // 'D'
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 134;
    if (i == 3) return 143;
    if (i == 4) return 141;
    if (i == 5) return 130;
    if (i == 6) return 100;
    return GLYPH_END;
}

int glyph_4(int i) {   // 'E'
    if (i == 0) return 44;
    if (i == 1) return 104;
    if (i == 2) return 100;
    if (i == 3) return 140;
    if (i == 4) return 2;
    if (i == 5) return 132;
    return GLYPH_END;
}

int glyph_5(int i) {   // 'F'
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 144;
    if (i == 3) return 2;
    if (i == 4) return 132;
    return GLYPH_END;
}

int glyph_6(int i) {   // 'G'
    if (i == 0) return 44;
    if (i == 1) return 114;
    if (i == 2) return 103;
    if (i == 3) return 101;
    if (i == 4) return 110;
    if (i == 5) return 140;
    if (i == 6) return 142;
    if (i == 7) return 122;
    return GLYPH_END;
}

int glyph_7(int i) {   // 'H'
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 2;
    if (i == 3) return 142;
    if (i == 4) return 44;
    if (i == 5) return 140;
    return GLYPH_END;
}

int glyph_8(int i) {   // 'I'
    if (i == 0) return 0;
    if (i == 1) return 104;
    return GLYPH_END;
}

int glyph_9(int i) {   // 'J'
    if (i == 0) return 1;
    if (i == 1) return 110;
    if (i == 2) return 130;
    if (i == 3) return 141;
    if (i == 4) return 144;
    return GLYPH_END;
}

int glyph_10(int i) {   // 'K'
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 2;
    if (i == 3) return 144;
    if (i == 4) return 2;
    if (i == 5) return 140;
    return GLYPH_END;
}

int glyph_11(int i) {   // 'L'
    if (i == 0) return 4;
    if (i == 1) return 100;
    if (i == 2) return 140;
    return GLYPH_END;
}

int glyph_12(int i) {   // 'M'
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 122;
    if (i == 3) return 144;
    if (i == 4) return 140;
    return GLYPH_END;
}

int glyph_13(int i) {   // 'N'
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 140;
    if (i == 3) return 144;
    return GLYPH_END;
}

int glyph_14(int i) {   // 'O'
    if (i == 0) return 10;
    if (i == 1) return 101;
    if (i == 2) return 103;
    if (i == 3) return 114;
    if (i == 4) return 134;
    if (i == 5) return 143;
    if (i == 6) return 141;
    if (i == 7) return 130;
    if (i == 8) return 110;
    return GLYPH_END;
}

int glyph_15(int i) {   // 'P'
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 134;
    if (i == 3) return 143;
    if (i == 4) return 132;
    if (i == 5) return 102;
    return GLYPH_END;
}

int glyph_16(int i) {   // 'Q'
    if (i == 0) return 10;
    if (i == 1) return 101;
    if (i == 2) return 103;
    if (i == 3) return 114;
    if (i == 4) return 134;
    if (i == 5) return 143;
    if (i == 6) return 141;
    if (i == 7) return 130;
    if (i == 8) return 110;
    if (i == 9) return 21;
    if (i == 10) return 140;
    return GLYPH_END;
}

int glyph_17(int i) {   // 'R'
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 134;
    if (i == 3) return 143;
    if (i == 4) return 132;
    if (i == 5) return 102;
    if (i == 6) return 22;
    if (i == 7) return 140;
    return GLYPH_END;
}

int glyph_18(int i) {   // 'S'
    if (i == 0) return 1;
    if (i == 1) return 110;
    if (i == 2) return 130;
    if (i == 3) return 141;
    if (i == 4) return 132;
    if (i == 5) return 112;
    if (i == 6) return 103;
    if (i == 7) return 114;
    if (i == 8) return 134;
    if (i == 9) return 143;
    return GLYPH_END;
}

int glyph_19(int i) {   // 'T'
    if (i == 0) return 4;
    if (i == 1) return 144;
    if (i == 2) return 24;
    if (i == 3) return 120;
    return GLYPH_END;
}

int glyph_20(int i) {   // 'U'
    if (i == 0) return 4;
    if (i == 1) return 101;
    if (i == 2) return 110;
    if (i == 3) return 130;
    if (i == 4) return 141;
    if (i == 5) return 144;
    return GLYPH_END;
}

int glyph_21(int i) {   // 'V'
    if (i == 0) return 4;
    if (i == 1) return 120;
    if (i == 2) return 144;
    return GLYPH_END;
}

int glyph_22(int i) {   // 'W'
    if (i == 0) return 4;
    if (i == 1) return 110;
    if (i == 2) return 122;
    if (i == 3) return 130;
    if (i == 4) return 144;
    return GLYPH_END;
}

int glyph_23(int i) {   // 'X'
    if (i == 0) return 0;
    if (i == 1) return 144;
    if (i == 2) return 4;
    if (i == 3) return 140;
    return GLYPH_END;
}

int glyph_24(int i) {   // 'Y'
    if (i == 0) return 4;
    if (i == 1) return 122;
    if (i == 2) return 144;
    if (i == 3) return 22;
    if (i == 4) return 120;
    return GLYPH_END;
}

int glyph_25(int i) {   // 'Z'
    if (i == 0) return 4;
    if (i == 1) return 144;
    if (i == 2) return 100;
    if (i == 3) return 140;
    return GLYPH_END;
}

int glyph_26(int i) {   // '0'
    if (i == 0) return 10;
    if (i == 1) return 101;
    if (i == 2) return 103;
    if (i == 3) return 114;
    if (i == 4) return 134;
    if (i == 5) return 143;
    if (i == 6) return 141;
    if (i == 7) return 130;
    if (i == 8) return 110;
    return GLYPH_END;
}

int glyph_27(int i) {   // '1'
    if (i == 0) return 3;
    if (i == 1) return 124;
    if (i == 2) return 120;
    return GLYPH_END;
}

int glyph_28(int i) {   // '2'
    if (i == 0) return 3;
    if (i == 1) return 114;
    if (i == 2) return 134;
    if (i == 3) return 143;
    if (i == 4) return 100;
    if (i == 5) return 140;
    return GLYPH_END;
}

int glyph_29(int i) {   // '3'
    if (i == 0) return 4;
    if (i == 1) return 144;
    if (i == 2) return 122;
    if (i == 3) return 141;
    if (i == 4) return 130;
    if (i == 5) return 110;
    if (i == 6) return 101;
    return GLYPH_END;
}

int glyph_30(int i) {   // '4'
    if (i == 0) return 30;
    if (i == 1) return 134;
    if (i == 2) return 101;
    if (i == 3) return 141;
    return GLYPH_END;
}

int glyph_31(int i) {   // '5'
    if (i == 0) return 44;
    if (i == 1) return 104;
    if (i == 2) return 102;
    if (i == 3) return 132;
    if (i == 4) return 141;
    if (i == 5) return 130;
    if (i == 6) return 110;
    if (i == 7) return 101;
    return GLYPH_END;
}

int glyph_32(int i) {   // '6'
    if (i == 0) return 44;
    if (i == 1) return 114;
    if (i == 2) return 103;
    if (i == 3) return 101;
    if (i == 4) return 110;
    if (i == 5) return 130;
    if (i == 6) return 141;
    if (i == 7) return 132;
    if (i == 8) return 102;
    return GLYPH_END;
}

int glyph_33(int i) {   // '7'
    if (i == 0) return 4;
    if (i == 1) return 144;
    if (i == 2) return 110;
    return GLYPH_END;
}

int glyph_34(int i) {   // '8'
    if (i == 0) return 12;
    if (i == 1) return 103;
    if (i == 2) return 114;
    if (i == 3) return 134;
    if (i == 4) return 143;
    if (i == 5) return 132;
    if (i == 6) return 112;
    if (i == 7) return 101;
    if (i == 8) return 110;
    if (i == 9) return 130;
    if (i == 10) return 141;
    if (i == 11) return 132;
    return GLYPH_END;
}

int glyph_35(int i) {   // '9'
    if (i == 0) return 0;
    if (i == 1) return 130;
    if (i == 2) return 141;
    if (i == 3) return 143;
    if (i == 4) return 134;
    if (i == 5) return 114;
    if (i == 6) return 103;
    if (i == 7) return 112;
    if (i == 8) return 142;
    return GLYPH_END;
}

int glyph_36(int i) {   // '-'
    if (i == 0) return 2;
    if (i == 1) return 142;
    return GLYPH_END;
}

int glyph_37(int i) {   // '.'
    if (i == 0) return 0;
    if (i == 1) return 222;
    return GLYPH_END;
}

int glyph_38(int i) {   // '!'
    if (i == 0) return 1;
    if (i == 1) return 104;
    if (i == 2) return 0;
    if (i == 3) return 222;
    return GLYPH_END;
}

int glyph_39(int i) {   // '?'
    if (i == 0) return 3;
    if (i == 1) return 114;
    if (i == 2) return 134;
    if (i == 3) return 143;
    if (i == 4) return 122;
    if (i == 5) return 121;
    if (i == 6) return 20;
    if (i == 7) return 222;
    return GLYPH_END;
}

int glyph(int slot, int i) {
    switch (slot) {
    case 0: return glyph_0(i);
    case 1: return glyph_1(i);
    case 2: return glyph_2(i);
    case 3: return glyph_3(i);
    case 4: return glyph_4(i);
    case 5: return glyph_5(i);
    case 6: return glyph_6(i);
    case 7: return glyph_7(i);
    case 8: return glyph_8(i);
    case 9: return glyph_9(i);
    case 10: return glyph_10(i);
    case 11: return glyph_11(i);
    case 12: return glyph_12(i);
    case 13: return glyph_13(i);
    case 14: return glyph_14(i);
    case 15: return glyph_15(i);
    case 16: return glyph_16(i);
    case 17: return glyph_17(i);
    case 18: return glyph_18(i);
    case 19: return glyph_19(i);
    case 20: return glyph_20(i);
    case 21: return glyph_21(i);
    case 22: return glyph_22(i);
    case 23: return glyph_23(i);
    case 24: return glyph_24(i);
    case 25: return glyph_25(i);
    case 26: return glyph_26(i);
    case 27: return glyph_27(i);
    case 28: return glyph_28(i);
    case 29: return glyph_29(i);
    case 30: return glyph_30(i);
    case 31: return glyph_31(i);
    case 32: return glyph_32(i);
    case 33: return glyph_33(i);
    case 34: return glyph_34(i);
    case 35: return glyph_35(i);
    case 36: return glyph_36(i);
    case 37: return glyph_37(i);
    case 38: return glyph_38(i);
    case 39: return glyph_39(i);
    default: return GLYPH_END;
    }
}

// ------------------------------------------------------------- motors -----
//
// ardio's stepper runtime keeps its state at one fixed block of SRAM
// addresses and can therefore only drive a single motor; this machine has
// two. So the four-step full-drive sequence is done here: the energised coil
// pair walks one coil at a time, and running the phase index downwards
// reverses the motor.

// Bit 0 is coil 1, bit 3 is coil 4.
int coil_pattern(int phase) {
    switch (phase) {
    case 1: return 6;                   // coils 2 and 3
    case 2: return 12;                  // coils 3 and 4
    case 3: return 9;                   // coils 4 and 1
    default: return 3;                  // coils 1 and 2
    }
}

void x_step(int dir) {
    x_phase = (x_phase + dir) & 3;
    int p = coil_pattern(x_phase);
    digitalWrite(X_PIN1, p & 1);
    digitalWrite(X_PIN2, (p >> 1) & 1);
    digitalWrite(X_PIN3, (p >> 2) & 1);
    digitalWrite(X_PIN4, (p >> 3) & 1);
    delay(X_STEP_MS);
}

void y_step(int dir) {
    y_phase = (y_phase + dir) & 3;
    int p = coil_pattern(y_phase);
    digitalWrite(Y_PIN1, p & 1);
    digitalWrite(Y_PIN2, (p >> 1) & 1);
    digitalWrite(Y_PIN3, (p >> 2) & 1);
    digitalWrite(Y_PIN4, (p >> 3) & 1);
    delay(Y_STEP_MS);
}

void y_run(int steps) {
    int dir = 1;
    if (steps < 0) { dir = -1; steps = -steps; }
    for (int i = 0; i < steps; i++) y_step(dir);
}

// Dropping every coil stops the motors cooking while the machine sits idle.
void release_motors() {
    digitalWrite(X_PIN1, 0); digitalWrite(X_PIN2, 0);
    digitalWrite(X_PIN3, 0); digitalWrite(X_PIN4, 0);
    digitalWrite(Y_PIN1, 0); digitalWrite(Y_PIN2, 0);
    digitalWrite(Y_PIN3, 0); digitalWrite(Y_PIN4, 0);
}

// ---------------------------------------------------------------- pen -----

void plot(bool down) {
    if (down) angle = PEN_DOWN_ANGLE;
    else angle = PEN_UP_ANGLE;
    servo_write(angle);
    if (down != pen_on_paper) delay(50);   // let the servo actually get there
    pen_on_paper = down;
}

void pen_up() { servo_write(PEN_UP_ANGLE); }
void pen_down() { servo_write(PEN_DOWN_ANGLE); }

void home_y_axis() {
    y_run(-3000);                          // wind the carriage down to the stop
}

// ---------------------------------------------------------------- line -----
//
// Bresenham: step whichever axis has further to go once per iteration, and let
// the error term decide when the other axis catches up.

void line(int newx, int newy, bool drawing) {
    plot(drawing);                         // pen down for a stroke, up for a move

    int dx = newx - xpos;
    int dy = newy - ypos;
    int dirx = 1;
    int diry = -1;
    if (dx > 0) dirx = -1;                 // the X wheel turns the other way
    if (dy > 0) diry = 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    if (dx > dy) {
        int over = dx / 2;
        for (int i = 0; i < dx; i++) {
            x_step(dirx);
            over = over + dy;
            if (over >= dx) {
                over = over - dx;
                y_step(diry);
            }
        }
    } else {
        int over = dy / 2;
        for (int i = 0; i < dy; i++) {
            y_step(diry);
            over = over + dx;
            if (over >= dy) {
                over = over - dy;
                x_step(dirx);
            }
        }
    }

    xpos = newx;
    ypos = newy;
}

// -------------------------------------------------------------- glyphs -----

void plot_character(int c, int x, int y) {
    int slot = glyph_slot(c);
    if (slot < 0) return;

    for (int i = 0; i < 14; i++) {
        int v = glyph(slot, i);
        if (v == GLYPH_END) return;

        if (v == GLYPH_DOT) {              // a full stop, and the dot of a '!'
            plot(true);
            delay(50);
            plot(false);
            continue;
        }

        bool draw = false;
        if (v > 99) {
            draw = true;
            v = v - 100;
        }
        int cx = v / 10;
        int cy = v % 10;
        // Y is multiplied by 7/2 because the lead screw covers about 3.5 times
        // less distance per step than the X wheel does.
        line(x + cx * x_scale, y + cy * y_scale * 7 / 2, draw);
    }
}

void plot_text(int x, int y) {
    int pos = 0;
    for (int i = 0; i < text_len; i++) {
        if (text[i] != 32) plot_character(text[i], x + pos, y);
        pos = pos + space;                 // a space just advances the carriage
    }
    release_motors();
}

// ---------------------------------------------------------------- setup -----

void setup() {
    i2c_init();
    lcd_init(LCD_ADDR, 16, 2);
    lcd_backlight(1);
    lcd_set_cursor(0, 0);
    print_str("Initializing... ");

    pinMode(X_PIN1, OUTPUT_MODE); pinMode(X_PIN2, OUTPUT_MODE);
    pinMode(X_PIN3, OUTPUT_MODE); pinMode(X_PIN4, OUTPUT_MODE);
    pinMode(Y_PIN1, OUTPUT_MODE); pinMode(Y_PIN2, OUTPUT_MODE);
    pinMode(Y_PIN3, OUTPUT_MODE); pinMode(Y_PIN4, OUTPUT_MODE);

    button_init(BTN_OK, DEBOUNCE_MS);      // debounce stops one click counting twice
    button_init(BTN_UP, DEBOUNCE_MS);
    button_init(BTN_DOWN, DEBOUNCE_MS);
    button_init(BTN_LEFT, DEBOUNCE_MS);
    button_init(BTN_RIGHT, DEBOUNCE_MS);

    servo_attach(SERVO_PIN);
    servo_write(angle);
    plot(false);                           // pen clear of the tape, so one can be loaded

    pen_up();
    home_y_axis();
    xpos = 0;
    ypos = 0;

    text_clear();
    release_motors();
    lcd_clear();
}

// ----------------------------------------------------------------- loop -----

void loop() {
    int ok = button_pressed(BTN_OK);
    int up = button_pressed(BTN_UP);
    int down = button_pressed(BTN_DOWN);
    int left = button_pressed(BTN_LEFT);
    int right = button_pressed(BTN_RIGHT);

    // The original blinks on millis() % 600; there is no millis() in the
    // runtime, so the cursor blinks on a pass counter instead.
    blink_tick = (blink_tick + 1) % 12;
    blink_on = blink_tick < 8;

    switch (state) {

    case MAIN_MENU:
        if (prev_state != MAIN_MENU) {
            lcd_clear();
            lcd_set_cursor(0, 0);
            print_str("   LABELMAKER   ");
            lcd_set_cursor(0, 1);
            print_str("      START     ");
            cursor_position = 5;
            prev_state = MAIN_MENU;
        }

        lcd_set_cursor(cursor_position, 1);
        if (blink_on) lcd_write_char(62);   // '>'
        else lcd_write_char(32);

        if (ok) {
            lcd_clear();
            state = EDITING;
            prev_state = MAIN_MENU;
        }
        break;

    // Up and down scroll the character under the cursor, left deletes, right
    // accepts and moves on, and the click finishes the label.
    case EDITING:
        if (prev_state != EDITING) {
            lcd_clear();
            prev_state = EDITING;
        }
        show_text();

        if (up) {
            if (current_character > 0) current_character = current_character - 1;
            lcd_write_char(alphabet(current_character));
            delay(250);                     // slow the scroll to a readable rate
        } else if (down) {
            if (current_character < ALPHABET_SIZE - 1) current_character = current_character + 1;
            lcd_write_char(alphabet(current_character));
            delay(250);
        } else {
            if (blink_on) lcd_write_char(alphabet(current_character));
            else lcd_write_char(32);
        }

        if (left) {
            text_backspace();
            show_text();
            delay(250);
        } else if (right) {
            text_append(alphabet(current_character));
            current_character = 0;
            delay(250);
        }

        if (ok) {
            text_append(alphabet(current_character));
            current_character = 0;
            lcd_clear();
            state = PRINT_CONFIRM;
            prev_state = EDITING;
        }
        break;

    case PRINT_CONFIRM:
        if (prev_state == EDITING) {
            lcd_set_cursor(0, 0);
            print_str("  PRINT LABEL?  ");
            lcd_set_cursor(0, 1);
            print_str("   YES     NO   ");
            cursor_position = 2;
            prev_state = PRINT_CONFIRM;
        }

        if (left) {
            lcd_set_cursor(0, 1);
            print_str("   YES     NO   ");
            cursor_position = 2;
            delay(200);
        } else if (right) {
            lcd_set_cursor(0, 1);
            print_str("   YES     NO   ");
            cursor_position = 10;
            delay(200);
        }

        lcd_set_cursor(cursor_position, 1);
        if (blink_on) lcd_write_char(62);
        else lcd_write_char(32);

        if (ok) {
            if (cursor_position == 2) {     // YES
                lcd_clear();
                state = PRINTING;
                prev_state = PRINT_CONFIRM;
            } else if (cursor_position == 10) {
                lcd_clear();
                state = EDITING;
                prev_state = PRINT_CONFIRM;
            }
        }
        break;

    case PRINTING:
        if (prev_state == PRINT_CONFIRM) {
            lcd_set_cursor(0, 0);
            print_str("    PRINTING    ");
        }

        plot_text(xpos, ypos);
        line(xpos + space, 0, false);       // run past the end of the label
        xpos = 0;
        ypos = 0;

        text_clear();
        y_run(-2250);                       // wind the tape back out
        release_motors();
        lcd_clear();
        state = EDITING;
        prev_state = PRINTING;
        break;

    default:
        state = MAIN_MENU;
        break;
    }
}

