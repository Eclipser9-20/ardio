; Blink the on-board LED (pin 13 = PB5) on an ATmega328P.
;
; Assembled by ardio itself -- no external toolchain required:
;
;     ardio push examples/blink.S
;
; The delay is a plain nested countdown rather than a timer, which keeps this
; to instructions the assembler already covers.

        sbi     0x04, 5         ; DDRB |= (1<<5)   -- pin 13 is an output

loop:
        sbi     0x05, 5         ; PORTB |= (1<<5)  -- LED on
        rcall   delay
        cbi     0x05, 5         ; PORTB &= ~(1<<5) -- LED off
        rcall   delay
        rjmp    loop

; Burns roughly a quarter second at 16 MHz: 256 * 256 * ~5 cycles.
delay:
        ldi     r18, 255
outer:
        ldi     r19, 255
inner:
        dec     r19
        brne    inner
        dec     r18
        brne    outer
        ret
