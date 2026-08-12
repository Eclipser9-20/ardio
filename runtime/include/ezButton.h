// ezButton.h — ardio's own debounced-button class, API-compatible with the
// commonly used ezButton sketch interface.
//
// The pin sampling and the debounce timer live in ardio's assembly runtime
// (runtime/button.S). The pin is configured with its internal pull-up enabled,
// so a button is wired between the pin and ground and reads LOW when pressed.
//
// ---------------------------------------------------------------------------
// WHAT THE RUNTIME ACTUALLY PROVIDES
//
// runtime/button.S exports two labels:
//
//     button_init(pin)      -- input with the pull-up on
//     button_pressed(pin)   -- 1 exactly once per physical press, then 0
//                              until the button is released again
//
// button_pressed does the whole debounce itself: it samples, waits about 5 ms,
// resamples, and latches, so it already reports one event per press however
// often it is polled. The level between events comes from digitalRead in
// runtime/core.S. Every method below is an inline forward to those three --
// ardio has no linker, so a method with no body fails at assembly time as
// "nothing implements 'ezButton__loop'" rather than at compile time.
//
// Divergences from the published API:
//
//   * setDebounceTime() is accepted and remembered, but the debounce window is
//     the runtime's fixed ~5 ms resample; the value has no effect on timing.
//     It is kept so that existing sketches compile unchanged, and getDebounce-
//     Time() reports back what was set.
//   * getCount() returns an int rather than unsigned long, since the code
//     generator's natural width here is 16 bits. The count wraps at 32767.
//   * The pin is claimed by button_init() in the constructor, which runs before
//     setup(). That is fine on this part: the port registers are live out of
//     reset.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_EZBUTTON_H
#define ARDIO_EZBUTTON_H

#include "Arduino.h"

// Registers a pin as an input with its internal pull-up engaged.
void button_init(int pin);

// 1 once for each press, then 0 until the button is released again.
int button_pressed(int pin);

class ezButton {
public:
    // The pin is set to input-with-pull-up. The debounce window is the
    // runtime's own; the 50 ms figure below is recorded only for
    // getDebounceTime().
    ezButton(int pin) {
        pin_ = (char)pin;
        debounce_ms_ = 50;
        state_ = (char)HIGH;
        last_state_ = (char)HIGH;
        pressed_edge_ = 0;
        released_edge_ = 0;
        count_ = 0;
        button_init(pin);
    }

    // Recorded, not applied — see the divergence note above.
    void setDebounceTime(int ms) { debounce_ms_ = ms; }
    int getDebounceTime() { return debounce_ms_; }

    // Sample the pin. Call this once per iteration of the sketch's loop(),
    // before asking any of the query methods anything — they report what the
    // most recent call observed, not the live pin.
    void loop() {
        last_state_ = state_;
        pressed_edge_ = (char)button_pressed((int)pin_);
        if (pressed_edge_ != 0) {
            state_ = (char)LOW;             // a press was just confirmed
            count_ = count_ + 1;
        } else {
            state_ = (char)digitalRead((int)pin_);
        }
        released_edge_ = (last_state_ == (char)LOW && state_ != (char)LOW) ? 1 : 0;
    }

    // Edge queries: true for exactly one loop() iteration, on the iteration
    // where the transition was seen.
    bool isPressed() { return pressed_edge_ != 0; }
    bool isReleased() { return released_edge_ != 0; }

    // Debounced level, HIGH or LOW, as read from the pin (LOW while held,
    // given the pull-up wiring described above).
    int getState() { return state_; }

    // The level seen on the previous loop() iteration.
    int getStateRaw() { return last_state_; }

    // Number of press events since construction.
    int getCount() { return count_; }
    void resetCount() { count_ = 0; }

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
