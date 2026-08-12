// ezButton.h — ardio's own debounced-button class, API-compatible with the
// commonly used ezButton sketch interface.
//
// The pin sampling and the debounce timer live in ardio's assembly runtime
// (runtime/button.S). The pin is configured with its internal pull-up enabled,
// so a button is wired between the pin and ground and reads LOW when pressed.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_EZBUTTON_H
#define ARDIO_EZBUTTON_H

#include "Arduino.h"

extern "C" {
// Registers a pin as a debounced button and returns its slot index.
uint8_t button_init(uint8_t pin, uint16_t debounce_ms);
// Current debounced level for a registered pin: 1 while held, 0 otherwise.
uint8_t button_pressed(uint8_t pin);
}

class ezButton {
public:
    // The pin is set to INPUT_PULLUP and given a 50 ms default debounce window.
    ezButton(int pin);

    // Debounce window in milliseconds.
    void setDebounceTime(int ms);

    // Sample the pin. Call this once per iteration of loop(), before asking any
    // of the query methods anything — they report what the most recent loop()
    // observed, not the live pin.
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

    // Number of press events since construction.
    unsigned long getCount();
    void resetCount();

private:
    uint8_t  pin_;
    uint16_t debounce_ms_;
    uint8_t  state_;
    uint8_t  last_state_;
    uint8_t  pressed_edge_;
    uint8_t  released_edge_;
    uint32_t count_;
};

#endif // ARDIO_EZBUTTON_H
