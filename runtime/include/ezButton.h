// ezButton.h — ardio's own debounced-button class, API-compatible with the
// commonly used ezButton sketch interface.
//
// The pin sampling and the debounce timer live in ardio's assembly runtime
// (runtime/button.S). The pin is configured with its internal pull-up enabled,
// so a button is wired between the pin and ground and reads LOW when pressed.
//
// Written inside the C++ subset ardio's own compiler accepts; see Arduino.h for
// the full list. Here: no `extern "C"`, no typedefs, and getCount() returns a
// plain int rather than unsigned long, since the code generator evaluates
// expressions 16 bits wide and a 32-bit return would be truncated silently.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_EZBUTTON_H
#define ARDIO_EZBUTTON_H

// Registers a pin as a debounced button and returns its slot index.
int button_init(int pin, int debounce_ms);

// Current debounced level for a registered pin: 1 while held, 0 otherwise.
int button_pressed(int pin);

class ezButton {
public:
    // The pin is set to INPUT_PULLUP and given a 50 ms default debounce window.
    ezButton(int pin);

    // Debounce window in milliseconds.
    void setDebounceTime(int ms);

    // Sample the pin. Call this once per iteration of the sketch's loop(),
    // before asking any of the query methods anything — they report what the
    // most recent call observed, not the live pin.
    void loop();

    // Edge queries: true for exactly one loop() iteration, on the iteration
    // where the transition was seen.
    bool isPressed();
    bool isReleased();

    // Debounced level, HIGH or LOW, as read from the pin (LOW while held, given
    // the pull-up wiring described above).
    int getState();

    // The level seen on the previous loop() iteration.
    int getStateRaw();

    // Number of press events since construction. Divergence: unsigned long in
    // the published API; int here, so the count wraps at 32767.
    int getCount();
    void resetCount();

private:
    char pin_;
    int debounce_ms_;
    char state_;
    char last_state_;
    char pressed_edge_;
    char released_edge_;
    int count_;
};

#endif // ARDIO_EZBUTTON_H
