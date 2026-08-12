// Label Maker -- a CrunchLabs Hack Pack style label printer, written for
// ardio's own AVR compiler.
//
//     ardio build examples/label_maker/label_maker.ino
//
// Same machine and the same behaviour as the classic starter sketch: a 16x2
// I2C character LCD, a five-way control, two 28BYJ-48 steppers driving an X
// carriage and a Y lead screw, and a servo that lifts the pen off the tape.
// A four-state machine walks you from a main menu through character-by-
// character editing and a print confirmation into plotting the label with a
// Bresenham line routine and a stroke font.
//
// Everything below is written to what ardio's compiler actually supports
// today, which is a good deal less than C++: no arrays, no switch, no
// division, no 32-bit locals and no string literals in expressions. The
// workarounds those force are spelled out in README.md next to this file.
//
// The runtime entry points are declared here rather than pulled in from a
// header, because ardio's compiler does not process #include yet. Only
// core.S is appended to the generated assembly at build time, so the pin and
// delay primitives below are the whole of the runtime that a sketch can call;
// the I2C display, the servo and the debounced buttons are built out of them
// here in the sketch.

void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);
int  digitalRead(int pin);
void delay(int ms);
void delayMicroseconds(int us);

const int INPUT_MODE = 0;
const int OUTPUT_MODE = 1;
const int PULLUP_MODE = 2;

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

const int BTN_OK    = 14;       // A0, the joystick click
const int BTN_UP    = 15;       // A1
const int BTN_DOWN  = 16;       // A2
const int BTN_LEFT  = 17;       // A3
const int BTN_RIGHT = 10;

const int PIN_SDA = 18;         // A4
const int PIN_SCL = 19;         // A5
const int LCD_ADDR = 39;        // 0x27, the usual PCF8574 backpack address
const int PEN_UP_ANGLE   = 25;
const int PEN_DOWN_ANGLE = 80;

// Step timing. The original sets 10 and 12 rpm on the two steppers; at 2048
// steps per revolution that is about 2.9 ms and 2.4 ms per step, and this
// port drives the coils itself so the delay is written out directly.
const int X_STEP_MS = 3;
const int Y_STEP_MS = 2;

// ------------------------------------------------------------- geometry -----

const int x_scale = 230;        // motor steps per font coordinate unit
const int y_scale = 230;
const int space = 1150;         // x_scale * 5: one character cell

int xpos = 0;                   // current carriage position, in steps
int ypos = 0;
int angle = PEN_UP_ANGLE;
bool pen_on_paper = false;

int x_phase = 0;                // full-drive phase index of each motor
int y_phase = 0;

// ---------------------------------------------------------- menu state -----

const int MAIN_MENU = 0;
const int EDITING = 1;
const int PRINT_CONFIRM = 2;
const int PRINTING = 3;

int state = MAIN_MENU;
int prev_state = PRINTING;
int cursor_position = 0;
int current_character = 0;
int blink_tick = 0;

// ---------------------------------------------------------- label text -----
//
// ardio has no arrays, so the label is 16 separate globals behind a pair of
// accessor functions. text_len is how many of them are in use.

int text_len = 0;
int text_0 = 32;
int text_1 = 32;
int text_2 = 32;
int text_3 = 32;
int text_4 = 32;
int text_5 = 32;
int text_6 = 32;
int text_7 = 32;
int text_8 = 32;
int text_9 = 32;
int text_10 = 32;
int text_11 = 32;
int text_12 = 32;
int text_13 = 32;
int text_14 = 32;
int text_15 = 32;

int text_get(int i) {
    if (i == 0) return text_0;
    if (i == 1) return text_1;
    if (i == 2) return text_2;
    if (i == 3) return text_3;
    if (i == 4) return text_4;
    if (i == 5) return text_5;
    if (i == 6) return text_6;
    if (i == 7) return text_7;
    if (i == 8) return text_8;
    if (i == 9) return text_9;
    if (i == 10) return text_10;
    if (i == 11) return text_11;
    if (i == 12) return text_12;
    if (i == 13) return text_13;
    if (i == 14) return text_14;
    if (i == 15) return text_15;
    return 32;
}

void text_set(int i, int c) {
    if (i == 0) { text_0 = c; return; }
    if (i == 1) { text_1 = c; return; }
    if (i == 2) { text_2 = c; return; }
    if (i == 3) { text_3 = c; return; }
    if (i == 4) { text_4 = c; return; }
    if (i == 5) { text_5 = c; return; }
    if (i == 6) { text_6 = c; return; }
    if (i == 7) { text_7 = c; return; }
    if (i == 8) { text_8 = c; return; }
    if (i == 9) { text_9 = c; return; }
    if (i == 10) { text_10 = c; return; }
    if (i == 11) { text_11 = c; return; }
    if (i == 12) { text_12 = c; return; }
    if (i == 13) { text_13 = c; return; }
    if (i == 14) { text_14 = c; return; }
    if (i == 15) { text_15 = c; return; }
}

void text_clear() {
    text_len = 0;
    for (int i = 0; i < 16; i++) text_set(i, 32);
}

void text_append(int c) {
    if (text_len >= 16) return;
    text_set(text_len, c);
    text_len = text_len + 1;
}

void text_backspace() {
    if (text_len <= 0) return;
    text_len = text_len - 1;
    text_set(text_len, 32);
}

// The character menu you scroll through while editing.
const int ALPHABET_SIZE = 41;

int alphabet(int i) {
    if (i == 0) return 32;   // space
    if (i == 1) return 65;   // A
    if (i == 2) return 66;   // B
    if (i == 3) return 67;   // C
    if (i == 4) return 68;   // D
    if (i == 5) return 69;   // E
    if (i == 6) return 70;   // F
    if (i == 7) return 71;   // G
    if (i == 8) return 72;   // H
    if (i == 9) return 73;   // I
    if (i == 10) return 74;   // J
    if (i == 11) return 75;   // K
    if (i == 12) return 76;   // L
    if (i == 13) return 77;   // M
    if (i == 14) return 78;   // N
    if (i == 15) return 79;   // O
    if (i == 16) return 80;   // P
    if (i == 17) return 81;   // Q
    if (i == 18) return 82;   // R
    if (i == 19) return 83;   // S
    if (i == 20) return 84;   // T
    if (i == 21) return 85;   // U
    if (i == 22) return 86;   // V
    if (i == 23) return 87;   // W
    if (i == 24) return 88;   // X
    if (i == 25) return 89;   // Y
    if (i == 26) return 90;   // Z
    if (i == 27) return 48;   // 0
    if (i == 28) return 49;   // 1
    if (i == 29) return 50;   // 2
    if (i == 30) return 51;   // 3
    if (i == 31) return 52;   // 4
    if (i == 32) return 53;   // 5
    if (i == 33) return 54;   // 6
    if (i == 34) return 55;   // 7
    if (i == 35) return 56;   // 8
    if (i == 36) return 57;   // 9
    if (i == 37) return 45;   // -
    if (i == 38) return 46;   // .
    if (i == 39) return 33;   // !
    if (i == 40) return 63;   // ?
    return 32;
}

// ------------------------------------------------------- integer maths -----
//
// ardio's code generator has no '/' or '%' yet, so the two divisions this
// sketch needs are done by hand. Both operands here are small and positive.

int div_small(int a, int b) {
    int q = 0;
    if (b <= 0) return 0;
    while (a >= b) {
        a = a - b;
        q = q + 1;
    }
    return q;
}

int abs_i(int v) {
    if (v < 0) return -v;
    return v;
}

// ----------------------------------------------------------- the font -----
//
// A stroke font written for this port. Each entry encodes one move of the
// pen: hundreds digit = pen down while moving, tens = x, ones = y, both in
// the 0..4 grid the scale factors multiply up into motor steps. 200 ends
// the glyph and 222 stamps a single dot.

const int GLYPH_END = 200;
const int GLYPH_DOT = 222;

// Maps an ASCII code to a glyph slot, or -1 for "nothing to draw".
int glyph_slot(int c) {
    if (c > 96 && c < 123) c = c - 32;      // fold lower case up
    if (c == 33) return 0;   // !
    if (c == 45) return 1;   // -
    if (c == 46) return 2;   // .
    if (c == 48) return 3;   // 0
    if (c == 49) return 4;   // 1
    if (c == 50) return 5;   // 2
    if (c == 51) return 6;   // 3
    if (c == 52) return 7;   // 4
    if (c == 53) return 8;   // 5
    if (c == 54) return 9;   // 6
    if (c == 55) return 10;   // 7
    if (c == 56) return 11;   // 8
    if (c == 57) return 12;   // 9
    if (c == 63) return 13;   // ?
    if (c == 65) return 14;   // A
    if (c == 66) return 15;   // B
    if (c == 67) return 16;   // C
    if (c == 68) return 17;   // D
    if (c == 69) return 18;   // E
    if (c == 70) return 19;   // F
    if (c == 71) return 20;   // G
    if (c == 72) return 21;   // H
    if (c == 73) return 22;   // I
    if (c == 74) return 23;   // J
    if (c == 75) return 24;   // K
    if (c == 76) return 25;   // L
    if (c == 77) return 26;   // M
    if (c == 78) return 27;   // N
    if (c == 79) return 28;   // O
    if (c == 80) return 29;   // P
    if (c == 81) return 30;   // Q
    if (c == 82) return 31;   // R
    if (c == 83) return 32;   // S
    if (c == 84) return 33;   // T
    if (c == 85) return 34;   // U
    if (c == 86) return 35;   // V
    if (c == 87) return 36;   // W
    if (c == 88) return 37;   // X
    if (c == 89) return 38;   // Y
    if (c == 90) return 39;   // Z
    return -1;
}

int glyph_0(int i) {   // !
    if (i == 0) return 1;
    if (i == 1) return 104;
    if (i == 2) return 0;
    if (i == 3) return 222;
    return GLYPH_END;
}

int glyph_1(int i) {   // -
    if (i == 0) return 2;
    if (i == 1) return 142;
    return GLYPH_END;
}

int glyph_2(int i) {   // .
    if (i == 0) return 0;
    if (i == 1) return 222;
    return GLYPH_END;
}

int glyph_3(int i) {   // 0
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

int glyph_4(int i) {   // 1
    if (i == 0) return 3;
    if (i == 1) return 124;
    if (i == 2) return 120;
    return GLYPH_END;
}

int glyph_5(int i) {   // 2
    if (i == 0) return 3;
    if (i == 1) return 114;
    if (i == 2) return 134;
    if (i == 3) return 143;
    if (i == 4) return 100;
    if (i == 5) return 140;
    return GLYPH_END;
}

int glyph_6(int i) {   // 3
    if (i == 0) return 4;
    if (i == 1) return 144;
    if (i == 2) return 122;
    if (i == 3) return 141;
    if (i == 4) return 130;
    if (i == 5) return 110;
    if (i == 6) return 101;
    return GLYPH_END;
}

int glyph_7(int i) {   // 4
    if (i == 0) return 30;
    if (i == 1) return 134;
    if (i == 2) return 101;
    if (i == 3) return 141;
    return GLYPH_END;
}

int glyph_8(int i) {   // 5
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

int glyph_9(int i) {   // 6
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

int glyph_10(int i) {   // 7
    if (i == 0) return 4;
    if (i == 1) return 144;
    if (i == 2) return 110;
    return GLYPH_END;
}

int glyph_11(int i) {   // 8
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

int glyph_12(int i) {   // 9
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

int glyph_13(int i) {   // ?
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

int glyph_14(int i) {   // A
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

int glyph_15(int i) {   // B
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

int glyph_16(int i) {   // C
    if (i == 0) return 44;
    if (i == 1) return 114;
    if (i == 2) return 103;
    if (i == 3) return 101;
    if (i == 4) return 110;
    if (i == 5) return 140;
    return GLYPH_END;
}

int glyph_17(int i) {   // D
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 134;
    if (i == 3) return 143;
    if (i == 4) return 141;
    if (i == 5) return 130;
    if (i == 6) return 100;
    return GLYPH_END;
}

int glyph_18(int i) {   // E
    if (i == 0) return 44;
    if (i == 1) return 104;
    if (i == 2) return 100;
    if (i == 3) return 140;
    if (i == 4) return 2;
    if (i == 5) return 132;
    return GLYPH_END;
}

int glyph_19(int i) {   // F
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 144;
    if (i == 3) return 2;
    if (i == 4) return 132;
    return GLYPH_END;
}

int glyph_20(int i) {   // G
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

int glyph_21(int i) {   // H
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 2;
    if (i == 3) return 142;
    if (i == 4) return 44;
    if (i == 5) return 140;
    return GLYPH_END;
}

int glyph_22(int i) {   // I
    if (i == 0) return 0;
    if (i == 1) return 104;
    return GLYPH_END;
}

int glyph_23(int i) {   // J
    if (i == 0) return 1;
    if (i == 1) return 110;
    if (i == 2) return 130;
    if (i == 3) return 141;
    if (i == 4) return 144;
    return GLYPH_END;
}

int glyph_24(int i) {   // K
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 2;
    if (i == 3) return 144;
    if (i == 4) return 2;
    if (i == 5) return 140;
    return GLYPH_END;
}

int glyph_25(int i) {   // L
    if (i == 0) return 4;
    if (i == 1) return 100;
    if (i == 2) return 140;
    return GLYPH_END;
}

int glyph_26(int i) {   // M
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 122;
    if (i == 3) return 144;
    if (i == 4) return 140;
    return GLYPH_END;
}

int glyph_27(int i) {   // N
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 140;
    if (i == 3) return 144;
    return GLYPH_END;
}

int glyph_28(int i) {   // O
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

int glyph_29(int i) {   // P
    if (i == 0) return 0;
    if (i == 1) return 104;
    if (i == 2) return 134;
    if (i == 3) return 143;
    if (i == 4) return 132;
    if (i == 5) return 102;
    return GLYPH_END;
}

int glyph_30(int i) {   // Q
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

int glyph_31(int i) {   // R
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

int glyph_32(int i) {   // S
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

int glyph_33(int i) {   // T
    if (i == 0) return 4;
    if (i == 1) return 144;
    if (i == 2) return 24;
    if (i == 3) return 120;
    return GLYPH_END;
}

int glyph_34(int i) {   // U
    if (i == 0) return 4;
    if (i == 1) return 101;
    if (i == 2) return 110;
    if (i == 3) return 130;
    if (i == 4) return 141;
    if (i == 5) return 144;
    return GLYPH_END;
}

int glyph_35(int i) {   // V
    if (i == 0) return 4;
    if (i == 1) return 120;
    if (i == 2) return 144;
    return GLYPH_END;
}

int glyph_36(int i) {   // W
    if (i == 0) return 4;
    if (i == 1) return 110;
    if (i == 2) return 122;
    if (i == 3) return 130;
    if (i == 4) return 144;
    return GLYPH_END;
}

int glyph_37(int i) {   // X
    if (i == 0) return 0;
    if (i == 1) return 144;
    if (i == 2) return 4;
    if (i == 3) return 140;
    return GLYPH_END;
}

int glyph_38(int i) {   // Y
    if (i == 0) return 4;
    if (i == 1) return 122;
    if (i == 2) return 144;
    if (i == 3) return 22;
    if (i == 4) return 120;
    return GLYPH_END;
}

int glyph_39(int i) {   // Z
    if (i == 0) return 4;
    if (i == 1) return 144;
    if (i == 2) return 100;
    if (i == 3) return 140;
    return GLYPH_END;
}

int glyph(int slot, int i) {
    if (slot == 0) return glyph_0(i);
    if (slot == 1) return glyph_1(i);
    if (slot == 2) return glyph_2(i);
    if (slot == 3) return glyph_3(i);
    if (slot == 4) return glyph_4(i);
    if (slot == 5) return glyph_5(i);
    if (slot == 6) return glyph_6(i);
    if (slot == 7) return glyph_7(i);
    if (slot == 8) return glyph_8(i);
    if (slot == 9) return glyph_9(i);
    if (slot == 10) return glyph_10(i);
    if (slot == 11) return glyph_11(i);
    if (slot == 12) return glyph_12(i);
    if (slot == 13) return glyph_13(i);
    if (slot == 14) return glyph_14(i);
    if (slot == 15) return glyph_15(i);
    if (slot == 16) return glyph_16(i);
    if (slot == 17) return glyph_17(i);
    if (slot == 18) return glyph_18(i);
    if (slot == 19) return glyph_19(i);
    if (slot == 20) return glyph_20(i);
    if (slot == 21) return glyph_21(i);
    if (slot == 22) return glyph_22(i);
    if (slot == 23) return glyph_23(i);
    if (slot == 24) return glyph_24(i);
    if (slot == 25) return glyph_25(i);
    if (slot == 26) return glyph_26(i);
    if (slot == 27) return glyph_27(i);
    if (slot == 28) return glyph_28(i);
    if (slot == 29) return glyph_29(i);
    if (slot == 30) return glyph_30(i);
    if (slot == 31) return glyph_31(i);
    if (slot == 32) return glyph_32(i);
    if (slot == 33) return glyph_33(i);
    if (slot == 34) return glyph_34(i);
    if (slot == 35) return glyph_35(i);
    if (slot == 36) return glyph_36(i);
    if (slot == 37) return glyph_37(i);
    if (slot == 38) return glyph_38(i);
    if (slot == 39) return glyph_39(i);
    return GLYPH_END;
}

// --------------------------------------------------------------- i2c -----
//
// A bit-banged two-wire master on A4/A5. The lines are open drain, so "high"
// means letting go of the line and leaving the module's pull-up to raise it;
// only the low level is ever driven.

void sda_high() { pinMode(PIN_SDA, PULLUP_MODE); delayMicroseconds(4); }
void sda_low()  { pinMode(PIN_SDA, OUTPUT_MODE); digitalWrite(PIN_SDA, 0); delayMicroseconds(4); }
void scl_high() { pinMode(PIN_SCL, PULLUP_MODE); delayMicroseconds(4); }
void scl_low()  { pinMode(PIN_SCL, OUTPUT_MODE); digitalWrite(PIN_SCL, 0); delayMicroseconds(4); }

void i2c_init() {
    sda_high();
    scl_high();
}

void i2c_start() {
    sda_high();
    scl_high();
    sda_low();
    scl_low();
}

void i2c_stop() {
    sda_low();
    scl_high();
    sda_high();
}

// One byte, most significant bit first, with the acknowledge clock ticked
// through and the slave's answer ignored -- there is nothing useful to do
// about a display that does not reply.
void i2c_write(int value) {
    for (int i = 0; i < 8; i++) {
        if ((value & 128) != 0) sda_high();
        else sda_low();
        scl_high();
        scl_low();
        value = value << 1;
    }
    sda_high();
    scl_high();
    scl_low();
}

void i2c_send(int value) {
    i2c_start();
    i2c_write(LCD_ADDR << 1);
    i2c_write(value);
    i2c_stop();
}

// --------------------------------------------------------------- lcd -----
//
// An HD44780 behind a PCF8574 expander: P0 is RS, P1 is R/W, P2 is the enable
// strobe, P3 drives the backlight transistor and P4..P7 carry one nibble of
// the bus at a time.

int lcd_backlight_bit = 8;

void lcd_backlight(int on) {
    if (on) lcd_backlight_bit = 8;
    else lcd_backlight_bit = 0;
    i2c_send(lcd_backlight_bit);
}

// Latches the top four bits of `bits` into the display. rs picks the data
// register over the instruction register.
void lcd_nibble(int bits, int rs) {
    int payload = (bits & 240) | lcd_backlight_bit | rs;
    i2c_send(payload);
    i2c_send(payload | 4);          // enable high
    delayMicroseconds(2);
    i2c_send(payload);              // and back low: the bus is sampled here
    delayMicroseconds(50);
}

void lcd_send(int value, int rs) {
    lcd_nibble(value, rs);
    lcd_nibble(value << 4, rs);
}

void lcd_command(int value) { lcd_send(value, 0); }
void lcd_write_char(int c) { lcd_send(c, 1); }

void lcd_clear() {
    lcd_command(1);
    delay(2);                       // clear is the one slow instruction
}

void lcd_set_cursor(int col, int row) {
    int base = 128;                 // DDRAM address 0, i.e. row 0
    if (row > 0) base = 192;        // row 1 starts at 0x40
    lcd_command(base + col);
}

void lcd_init() {
    i2c_init();
    delay(50);                      // the controller boots slowly after power-up
    lcd_nibble(48, 0);              // the 8-bit reset dance, three times
    delay(5);
    lcd_nibble(48, 0);
    delay(5);
    lcd_nibble(48, 0);
    delay(1);
    lcd_nibble(32, 0);              // and finally drop to the 4-bit bus
    delay(1);
    lcd_command(40);                // 4-bit, two lines, 5x8 font
    lcd_command(8);                 // display off
    lcd_command(1);                 // clear
    delay(2);
    lcd_command(6);                 // entry mode: advance, no shift
    lcd_command(12);                // display on, no cursor, no blink
    lcd_backlight(1);
}

// -------------------------------------------------------------- servo -----
//
// No timer peripheral is exposed yet, so the pen servo is driven by counting
// out its pulses: roughly 1000 us at 0 degrees to 1900 us at 180, repeated
// often enough for the horn to actually reach the angle.

void servo_attach(int pin) {
    pinMode(pin, OUTPUT_MODE);
    digitalWrite(pin, 0);
}

void servo_write(int a) {
    if (a < 0) a = 0;
    if (a > 180) a = 180;
    int width = 1000 + a * 5;
    for (int i = 0; i < 12; i++) {
        digitalWrite(SERVO_PIN, 1);
        delayMicroseconds(width);
        digitalWrite(SERVO_PIN, 0);
        delay(19);
    }
}

// ------------------------------------------------------------ buttons -----
//
// Five momentary contacts to ground with the internal pull-ups on, so a
// pressed key reads low. Each one gets an edge detector so that holding a
// direction does not repeat -- what ezButton's isPressed() gives you.

int prev_ok = 1;
int prev_up = 1;
int prev_down = 1;
int prev_left = 1;
int prev_right = 1;

int button_prev(int pin) {
    if (pin == BTN_OK) return prev_ok;
    if (pin == BTN_UP) return prev_up;
    if (pin == BTN_DOWN) return prev_down;
    if (pin == BTN_LEFT) return prev_left;
    if (pin == BTN_RIGHT) return prev_right;
    return 1;
}

void button_store(int pin, int level) {
    if (pin == BTN_OK) prev_ok = level;
    if (pin == BTN_UP) prev_up = level;
    if (pin == BTN_DOWN) prev_down = level;
    if (pin == BTN_LEFT) prev_left = level;
    if (pin == BTN_RIGHT) prev_right = level;
}

void button_init(int pin) {
    pinMode(pin, PULLUP_MODE);
    button_store(pin, 1);
}

// True on the falling edge only, after the contact has settled.
int button_pressed(int pin) {
    int level = digitalRead(pin);
    int was = button_prev(pin);
    if (level == was) return 0;
    delay(50);                      // debounce, as the original's 50 ms
    level = digitalRead(pin);
    button_store(pin, level);
    if (level == 0) return 1;
    return 0;
}

// ------------------------------------------------------------- screen -----
//
// String literals are not usable in expressions yet, so each fixed message
// is a function that pushes its characters at the display one at a time.

void lcd_msg_init() {   // "Initializing... "
    lcd_write_char(73);
    lcd_write_char(110);
    lcd_write_char(105);
    lcd_write_char(116);
    lcd_write_char(105);
    lcd_write_char(97);
    lcd_write_char(108);
    lcd_write_char(105);
    lcd_write_char(122);
    lcd_write_char(105);
    lcd_write_char(110);
    lcd_write_char(103);
    lcd_write_char(46);
    lcd_write_char(46);
    lcd_write_char(46);
    lcd_write_char(32);
}

void lcd_msg_name() {   // "   LABELMAKER   "
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(76);
    lcd_write_char(65);
    lcd_write_char(66);
    lcd_write_char(69);
    lcd_write_char(76);
    lcd_write_char(77);
    lcd_write_char(65);
    lcd_write_char(75);
    lcd_write_char(69);
    lcd_write_char(82);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
}

void lcd_msg_start() {   // "      START     "
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(83);
    lcd_write_char(84);
    lcd_write_char(65);
    lcd_write_char(82);
    lcd_write_char(84);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
}

void lcd_msg_confirm() {   // "  PRINT LABEL?  "
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(80);
    lcd_write_char(82);
    lcd_write_char(73);
    lcd_write_char(78);
    lcd_write_char(84);
    lcd_write_char(32);
    lcd_write_char(76);
    lcd_write_char(65);
    lcd_write_char(66);
    lcd_write_char(69);
    lcd_write_char(76);
    lcd_write_char(63);
    lcd_write_char(32);
    lcd_write_char(32);
}

void lcd_msg_yes_no() {   // "   YES     NO   "
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(89);
    lcd_write_char(69);
    lcd_write_char(83);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(78);
    lcd_write_char(79);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
}

void lcd_msg_printing() {   // "    PRINTING    "
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(80);
    lcd_write_char(82);
    lcd_write_char(73);
    lcd_write_char(78);
    lcd_write_char(84);
    lcd_write_char(73);
    lcd_write_char(78);
    lcd_write_char(71);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
}

void lcd_msg_clear_row() {   // ":               "
    lcd_write_char(58);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
    lcd_write_char(32);
}

void lcd_show_text() {
    lcd_set_cursor(0, 0);
    lcd_msg_clear_row();
    lcd_set_cursor(1, 0);
    for (int i = 0; i < text_len; i++) lcd_write_char(text_get(i));
}

// ------------------------------------------------------------- motors -----
//
// ardio's stepper runtime keeps its state at one fixed set of SRAM addresses,
// so it can only ever drive a single motor. This machine has two, so the
// four-step full-drive sequence is done here instead: the energised coil pair
// walks one coil at a time (1100, 0110, 0011, 1001), and running the phase
// index downwards reverses the motor.

// Bit 0 is coil 1 ... bit 3 is coil 4.
int coil_pattern(int phase) {
    if (phase == 1) return 6;              // 0110
    if (phase == 2) return 12;             // 0011 -> coils 3 and 4
    if (phase == 3) return 9;              // 1001
    return 3;                              // 1100 -> coils 1 and 2
}

void x_apply(int phase) {
    int p = coil_pattern(phase);
    digitalWrite(X_PIN1, p & 1);
    digitalWrite(X_PIN2, (p >> 1) & 1);
    digitalWrite(X_PIN3, (p >> 2) & 1);
    digitalWrite(X_PIN4, (p >> 3) & 1);
}

void y_apply(int phase) {
    int p = coil_pattern(phase);
    digitalWrite(Y_PIN1, p & 1);
    digitalWrite(Y_PIN2, (p >> 1) & 1);
    digitalWrite(Y_PIN3, (p >> 2) & 1);
    digitalWrite(Y_PIN4, (p >> 3) & 1);
}

void x_step(int dir) {
    x_phase = x_phase + dir;
    if (x_phase > 3) x_phase = 0;
    if (x_phase < 0) x_phase = 3;
    x_apply(x_phase);
    delay(X_STEP_MS);
}

void y_step(int dir) {
    y_phase = y_phase + dir;
    if (y_phase > 3) y_phase = 0;
    if (y_phase < 0) y_phase = 3;
    y_apply(y_phase);
    delay(Y_STEP_MS);
}

void y_run(int steps) {
    int dir = 1;
    if (steps < 0) dir = -1;
    steps = abs_i(steps);
    for (int i = 0; i < steps; i++) y_step(dir);
}

// Dropping every coil stops the motors cooking while the machine sits idle.
void release_motors() {
    digitalWrite(X_PIN1, 0); digitalWrite(X_PIN2, 0); digitalWrite(X_PIN3, 0); digitalWrite(X_PIN4, 0);
    digitalWrite(Y_PIN1, 0); digitalWrite(Y_PIN2, 0); digitalWrite(Y_PIN3, 0); digitalWrite(Y_PIN4, 0);
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
// Bresenham: step whichever axis moves further once per iteration and let the
// error term decide when the other axis catches up.

// The two halves differ only in which axis carries the loop; they are separate
// functions because the assembler's conditional branches reach 63 words and a
// loop body this size does not fit inside an if.
// One iteration each, returning the updated error term. Even a loop body of
// half a dozen statements overflows a branch, so the bodies are calls.
int x_major_tick(int over, int dx, int dy, int dirx, int diry) {
    x_step(dirx);
    over = over + dy;
    if (over < dx) return over;
    y_step(diry);
    return over - dx;
}

int y_major_tick(int over, int dx, int dy, int dirx, int diry) {
    y_step(diry);
    over = over + dx;
    if (over < dy) return over;
    x_step(dirx);
    return over - dy;
}

void run_x_major(int dx, int dy, int dirx, int diry) {
    int over = dx >> 1;
    for (int i = 0; i < dx; i++) over = x_major_tick(over, dx, dy, dirx, diry);
}

void run_y_major(int dx, int dy, int dirx, int diry) {
    int over = dy >> 1;
    for (int i = 0; i < dy; i++) over = y_major_tick(over, dx, dy, dirx, diry);
}

void line(int newx, int newy, bool drawing) {
    plot(drawing);

    int dx = newx - xpos;
    int dy = newy - ypos;
    int dirx = 1;
    int diry = -1;
    if (dx > 0) dirx = -1;                 // the X wheel turns the other way
    if (dy > 0) diry = 1;
    dx = abs_i(dx);
    dy = abs_i(dy);

    if (dx > dy) run_x_major(dx, dy, dirx, diry);
    else run_y_major(dx, dy, dirx, diry);

    xpos = newx;
    ypos = newy;
}

// --------------------------------------------------------------- glyphs -----

void dot() {
    plot(true);
    delay(50);
    plot(false);
}

// One encoded move of a glyph, resolved against the character's origin.
void glyph_move(int v, int x, int y) {
    bool draw = false;
    if (v > 99) { draw = true; v = v - 100; }
    int cx = div_small(v, 10);
    int cy = v - cx * 10;
    // Y is multiplied by 7/2 because the lead screw covers about 3.5 times
    // less distance per step than the X wheel does.
    line(x + cx * x_scale, y + ((cy * y_scale * 7) >> 1), draw);
}

void plot_character(int c, int x, int y) {
    int slot = glyph_slot(c);
    if (slot < 0) return;

    for (int i = 0; i < 14; i++) {
        int v = glyph(slot, i);
        if (v == GLYPH_END) return;
        if (v == GLYPH_DOT) dot();
        else glyph_move(v, x, y);
    }
}

void plot_text(int x, int y) {
    int pos = 0;
    for (int i = 0; i < text_len; i++) {
        int c = text_get(i);
        if (c != 32) plot_character(c, x + pos, y);
        pos = pos + space;
    }
    release_motors();
}

// ---------------------------------------------------------------- setup -----

void setup() {
    lcd_init();
    lcd_set_cursor(0, 0);
    lcd_msg_init();

    pinMode(X_PIN1, 1); pinMode(X_PIN2, 1); pinMode(X_PIN3, 1); pinMode(X_PIN4, 1);
    pinMode(Y_PIN1, 1); pinMode(Y_PIN2, 1); pinMode(Y_PIN3, 1); pinMode(Y_PIN4, 1);

    button_init(BTN_OK);
    button_init(BTN_UP);
    button_init(BTN_DOWN);
    button_init(BTN_LEFT);
    button_init(BTN_RIGHT);

    servo_attach(SERVO_PIN);
    servo_write(angle);
    plot(false);                           // park the pen clear of the tape

    pen_up();
    home_y_axis();
    xpos = 0;
    ypos = 0;

    text_clear();
    release_motors();
    lcd_clear();
}

// ------------------------------------------------------------- the menu -----
//
// The original is one switch over the state; ardio has no switch, so this is
// an if/else chain, and each arm is its own function because the assembler's
// conditional branches only reach 63 words.

int blink_on = 0;

void draw_main_menu() {
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_msg_name();
    lcd_set_cursor(0, 1);
    lcd_msg_start();
    cursor_position = 5;
    prev_state = MAIN_MENU;
}

void draw_cursor() {
    lcd_set_cursor(cursor_position, 1);
    if (blink_on) lcd_write_char(62);      // '>'
    else lcd_write_char(32);
}

void state_main_menu(int ok) {
    if (prev_state != MAIN_MENU) draw_main_menu();
    draw_cursor();
    if (!ok) return;
    lcd_clear();
    state = EDITING;
    prev_state = MAIN_MENU;
}

void char_prev() {
    if (current_character > 0) current_character = current_character - 1;
    lcd_write_char(alphabet(current_character));
    delay(250);                            // slow the scroll to a readable rate
}

void char_next() {
    if (current_character < ALPHABET_SIZE - 1) current_character = current_character + 1;
    lcd_write_char(alphabet(current_character));
    delay(250);
}

void char_blink() {
    if (blink_on) lcd_write_char(alphabet(current_character));
    else lcd_write_char(32);
}

void commit_character() {
    text_append(alphabet(current_character));
    current_character = 0;
}

void finish_editing() {
    commit_character();
    lcd_clear();
    state = PRINT_CONFIRM;
    prev_state = EDITING;
}

void state_editing(int ok, int up, int down, int left, int right) {
    if (prev_state != EDITING) { lcd_clear(); prev_state = EDITING; }
    lcd_show_text();

    if (up) char_prev();
    else if (down) char_next();
    else char_blink();

    if (left) { text_backspace(); lcd_show_text(); delay(250); }
    else if (right) { commit_character(); delay(250); }

    if (ok) finish_editing();
}

void draw_confirm() {
    lcd_set_cursor(0, 0);
    lcd_msg_confirm();
    lcd_set_cursor(0, 1);
    lcd_msg_yes_no();
    cursor_position = 2;
    prev_state = PRINT_CONFIRM;
}

void move_confirm_cursor(int col) {
    lcd_set_cursor(0, 1);
    lcd_msg_yes_no();
    cursor_position = col;
    delay(200);
}

void answer_confirm() {
    if (cursor_position == 2) state = PRINTING;
    else if (cursor_position == 10) state = EDITING;
    else return;
    lcd_clear();
    prev_state = PRINT_CONFIRM;
}

void state_confirm(int ok, int left, int right) {
    if (prev_state == EDITING) draw_confirm();

    if (left) move_confirm_cursor(2);
    else if (right) move_confirm_cursor(10);

    draw_cursor();
    if (ok) answer_confirm();
}

void state_printing() {
    if (prev_state == PRINT_CONFIRM) {
        lcd_set_cursor(0, 0);
        lcd_msg_printing();
    }

    plot_text(xpos, ypos);
    line(xpos + space, 0, false);          // feed past the end of the label
    xpos = 0;
    ypos = 0;

    text_clear();
    y_run(-2250);                          // wind the tape back out
    release_motors();
    lcd_clear();
    state = EDITING;
    prev_state = PRINTING;
}

// ----------------------------------------------------------------- loop -----

void loop() {
    int ok = button_pressed(BTN_OK);
    int up = button_pressed(BTN_UP);
    int down = button_pressed(BTN_DOWN);
    int left = button_pressed(BTN_LEFT);
    int right = button_pressed(BTN_RIGHT);

    // No millis() in the runtime, so the cursor blink is a loop counter.
    blink_tick = blink_tick + 1;
    if (blink_tick > 11) blink_tick = 0;
    blink_on = 0;
    if (blink_tick < 8) blink_on = 1;

    if (state == MAIN_MENU) state_main_menu(ok);
    else if (state == EDITING) state_editing(ok, up, down, left, right);
    else if (state == PRINT_CONFIRM) state_confirm(ok, left, right);
    else if (state == PRINTING) state_printing();
}

