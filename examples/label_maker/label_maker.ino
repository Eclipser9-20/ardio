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

const char OUTPUT_MODE = 1;

// ------------------------------------------------------------ hardware -----
//
// Pin numbers are tables rather than eight and five separate constants,
// because setup walks them in a loop and drive_coils walks four at a time.

// Coil 1 to coil 4 of the X carriage motor, then the same for the Y lead
// screw. X_MOTOR and Y_MOTOR are where each motor's four start.
const char motor_pins[8] = {6, 8, 7, 9, 2, 4, 3, 5};
const char X_MOTOR = 0;
const char Y_MOTOR = 4;

// The joystick of the original, as five contacts to ground: there is no
// analogRead in the runtime to read a real one with. A0 is the click, then
// A1, A2 and A3 for up, down and left, and D10 for right.
const char button_pins[5] = {14, 15, 16, 17, 10};
const char BTN_OK    = 14;
const char BTN_UP    = 15;
const char BTN_DOWN  = 16;
const char BTN_LEFT  = 17;
const char BTN_RIGHT = 10;
const char DEBOUNCE_MS = 50;

const char SERVO_PIN = 13;

const char LCD_ADDR = 39;     // 0x27, the usual PCF8574 backpack address
const char PEN_UP_ANGLE   = 25;
const char PEN_DOWN_ANGLE = 80;

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
bool pen_on_paper = false;

int x_phase = 0;                // full-drive phase index of each motor
int y_phase = 0;

// ---------------------------------------------------------- menu state -----

const char MAIN_MENU = 0;
const char EDITING = 1;
const char PRINT_CONFIRM = 2;
const char PRINTING = 3;

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

const char TEXT_MAX = 16;
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

// The second row of the print prompt, and the blinking '>' that both menus
// park on the current choice. Each was written out at three call sites.
void show_choices() {
    lcd_set_cursor(0, 1);
    print_str("   YES     NO   ");
}

void blink_cursor() {
    lcd_set_cursor(cursor_position, 1);
    if (blink_on) lcd_write_char(62);      // '>'
    else lcd_write_char(32);
}

// --------------------------------------------------- the editing menu -----
//
// Slot 0 is a space, then A..Z, then 0..9, then four marks. Written as
// arithmetic rather than a lookup table because on this compiler an array
// index costs more than the subtraction it would replace.

const char ALPHABET_SIZE = 41;

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
// Each byte encodes one move of the pen: bit 6 means draw while moving, bits
// 3..5 are x and bits 0..2 are y, both on the 0..4 grid that the scale factors
// multiply up into motor steps. 126 stamps a single dot instead of moving.
//
// The glyph shapes are the ones this sketch has always drawn; only the storage
// changed, from forty small functions behind a switch to one flat table.

const char GLYPH_DOT = 126;

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

// Every stroke of every glyph, end to end, one byte each. glyph_start says
// where each slot begins, and the slot after it says where it ends, so no
// terminator byte is needed.
const char stroke[242] = {
    0, 67, 76, 92, 99, 96, 2, 98,            // 'A'
    0, 68, 92, 99, 90, 66, 26, 97, 88, 64,   // 'B'
    36, 76, 67, 65, 72, 96,                  // 'C'
    0, 68, 92, 99, 97, 88, 64,               // 'D'
    36, 68, 64, 96, 2, 90,                   // 'E'
    0, 68, 100, 2, 90,                       // 'F'
    36, 76, 67, 65, 72, 96, 98, 82,          // 'G'
    0, 68, 2, 98, 36, 96,                    // 'H'
    0, 68,                                   // 'I'
    1, 72, 88, 97, 100,                      // 'J'
    0, 68, 2, 100, 2, 96,                    // 'K'
    4, 64, 96,                               // 'L'
    0, 68, 82, 100, 96,                      // 'M'
    0, 68, 96, 100,                          // 'N'
    8, 65, 67, 76, 92, 99, 97, 88, 72,       // 'O'
    0, 68, 92, 99, 90, 66,                   // 'P'
    8, 65, 67, 76, 92, 99, 97, 88, 72, 17, 96, // 'Q'
    0, 68, 92, 99, 90, 66, 18, 96,           // 'R'
    1, 72, 88, 97, 90, 74, 67, 76, 92, 99,   // 'S'
    4, 100, 20, 80,                          // 'T'
    4, 65, 72, 88, 97, 100,                  // 'U'
    4, 80, 100,                              // 'V'
    4, 72, 82, 88, 100,                      // 'W'
    0, 100, 4, 96,                           // 'X'
    4, 82, 100, 18, 80,                      // 'Y'
    4, 100, 64, 96,                          // 'Z'
    8, 65, 67, 76, 92, 99, 97, 88, 72,       // '0'
    3, 84, 80,                               // '1'
    3, 76, 92, 99, 64, 96,                   // '2'
    4, 100, 82, 97, 88, 72, 65,              // '3'
    24, 92, 65, 97,                          // '4'
    36, 68, 66, 90, 97, 88, 72, 65,          // '5'
    36, 76, 67, 65, 72, 88, 97, 90, 66,      // '6'
    4, 100, 72,                              // '7'
    10, 67, 76, 92, 99, 90, 74, 65, 72, 88, 97, 90, // '8'
    0, 88, 97, 99, 92, 76, 67, 74, 98,       // '9'
    2, 98,                                   // '-'
    0, 126,                                  // '.'
    1, 68, 0, 126,                           // '!'
    3, 76, 92, 99, 82, 81, 16, 126,          // '?'
};

// Where each slot starts in the table above, biased by -121: the table is
// 242 bytes long and a char only reaches 127, so the bias is what lets the
// offsets be bytes rather than words. The entry after a slot is where it
// ends, which is why there are forty-one of them.
const char glyph_start[41] = {
    -121, -113, -103, -97, -90, -84, -79, -71, -65, -63,
    -58, -52, -49, -44, -40, -31, -25, -14, -6, 4,
    8, 14, 17, 22, 26, 31, 35, 44, 47, 53,
    60, 64, 72, 81, 84, 96, 105, 107, 109, 113,
    121
};

// ------------------------------------------------------------- motors -----
//
// ardio's stepper runtime keeps its state at one fixed block of SRAM
// addresses and can therefore only drive a single motor; this machine has
// two. So the four-step full-drive sequence is done here: the energised coil
// pair walks one coil at a time, and running the phase index downwards
// reverses the motor.

// One phase per entry; bit 0 is coil 1 and bit 3 is coil 4, so each entry
// energises the pair of coils that phase drives.
const char coil_pattern[4] = {
    3,                                  // coils 1 and 2
    6,                                  // coils 2 and 3
    12,                                 // coils 3 and 4
    9                                   // coils 4 and 1
};

// The two motors differ only in where their four pins sit in motor_pins, so
// one routine drives either of them: base 0 is X, base 4 is Y.
void drive_coils(int base, int p) {
    for (int i = 0; i < 4; i++) digitalWrite(motor_pins[base + i], (p >> i) & 1);
}

void x_step(int dir) {
    x_phase = (x_phase + dir) & 3;
    drive_coils(X_MOTOR, coil_pattern[x_phase]);
    delay(X_STEP_MS);
}

void y_step(int dir) {
    y_phase = (y_phase + dir) & 3;
    drive_coils(Y_MOTOR, coil_pattern[y_phase]);
    delay(Y_STEP_MS);
}

void y_run(int steps) {
    int dir = 1;
    if (steps < 0) { dir = -1; steps = -steps; }
    for (int i = 0; i < steps; i++) y_step(dir);
}

// Dropping every coil stops the motors cooking while the machine sits idle.
void release_motors() {
    drive_coils(X_MOTOR, 0);
    drive_coils(Y_MOTOR, 0);
}

// ---------------------------------------------------------------- pen -----

void plot(bool down) {
    if (down) servo_write(PEN_DOWN_ANGLE);
    else servo_write(PEN_UP_ANGLE);
    if (down != pen_on_paper) delay(50);   // let the servo actually get there
    pen_on_paper = down;
}

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

    int end = glyph_start[slot + 1] + 121;
    for (int i = glyph_start[slot] + 121; i < end; i++) {
        int v = stroke[i];

        if (v == GLYPH_DOT) {              // a full stop, and the dot of a '!'
            plot(true);
            delay(50);
            plot(false);
            continue;
        }

        bool draw = false;
        if (v > 63) {
            draw = true;
            v = v - 64;
        }
        int cx = v / 8;
        int cy = v % 8;
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

    for (int i = 0; i < 8; i++) pinMode(motor_pins[i], OUTPUT_MODE);

    // Debounce stops one click counting twice.
    for (int i = 0; i < 5; i++) button_init(button_pins[i], DEBOUNCE_MS);

    servo_attach(SERVO_PIN);
    plot(false);                           // pen clear of the tape, so one can be loaded
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

        blink_cursor();

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
            show_choices();
            cursor_position = 2;
            prev_state = PRINT_CONFIRM;
        }

        if (left || right) {
            show_choices();
            if (left) cursor_position = 2;
            else cursor_position = 10;
            delay(200);
        }

        blink_cursor();

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

