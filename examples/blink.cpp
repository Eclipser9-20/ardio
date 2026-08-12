// Blink the on-board LED (pin 13 = PORTB bit 5) on an ATmega328P.
//
// This is bare-metal AVR: direct register access, no Arduino core library.
// ardio compiles a single translation unit with avr-g++ and links nothing else,
// so sketches must be self-contained.
//
//   ardio push examples/blink.cpp

#include <avr/io.h>
#include <util/delay.h>

int main() {
    DDRB |= (1 << DDB5);          // pin 13 as output

    for (;;) {
        PORTB |= (1 << PORTB5);   // on
        _delay_ms(500);
        PORTB &= ~(1 << PORTB5);  // off
        _delay_ms(500);
    }
}
