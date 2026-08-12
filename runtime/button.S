; ardio runtime -- debounced push-button input, active low.
;
; Buttons are wired the usual way: one side to the pin, one side to ground,
; with the ATmega328P's internal pull-up holding the pin high while the button
; is open. So "pressed" means the pin reads 0.
;
; Debouncing here is confirm-by-resample plus edge latching: the pin must read
; low, still read low ~5 ms later, and not already have been reported as down.
; That rejects contact bounce (which settles well inside a millisecond on these
; tactile switches) and makes button_pressed() fire exactly once per physical
; press no matter how often it is polled.
;
; Pin numbering follows the Nano silkscreen: 0-7 on PORTD, 8-13 on PORTB,
; 14-19 on PORTC. Each port's three registers are consecutive in data space
; (PINx, DDRx, PORTx), so one Z pointer at PINx reaches all three with the
; displacement forms Z+0, Z+1 and Z+2:
;
;     PIND 0x29  DDRD 0x2A  PORTD 0x2B
;     PINB 0x23  DDRB 0x24  PORTB 0x25
;     PINC 0x26  DDRC 0x27  PORTC 0x28
;
; (Those are data-space addresses -- the I/O-space numbers used by sbi/cbi are
; 0x20 lower.)
;
; SRAM state (documented so other runtime files do not collide):
;     0x0302  latched state, pins 0-7    (PORTD bank)
;     0x0303  latched state, pins 8-13   (PORTB bank)
;     0x0304  latched state, pins 14-19  (PORTC bank)
; (lcd.S owns 0x0300-0x0301. wire.S owns nothing.)
;
; Registers: avr-gcc convention. r16 and r17 are call-saved and are pushed by
; both public entry points.

; ---------------------------------------------------------------------------
; button_bank (internal) -- r24 = pin number.
;
; Returns Z = data address of that pin's PINx register, r17 = bit mask within
; the port, r16 = bank index (0 PORTD, 1 PORTB, 2 PORTC). An out-of-range pin
; comes back with Z = 0, which both callers treat as "do nothing".
; ---------------------------------------------------------------------------
button_bank:
        ldi     r31, 0                  ; every port register is in page 0
        cpi     r24, 8
        brlo    button_bank_d
        cpi     r24, 14
        brlo    button_bank_b
        cpi     r24, 20
        brlo    button_bank_c
        ldi     r30, 0                  ; unknown pin
        ret
button_bank_d:
        ldi     r30, 0x29               ; PIND
        ldi     r16, 0
        mov     r18, r24
        rjmp    button_bank_mask
button_bank_b:
        ldi     r30, 0x23               ; PINB
        ldi     r16, 1
        mov     r18, r24
        subi    r18, 8
        rjmp    button_bank_mask
button_bank_c:
        ldi     r30, 0x26               ; PINC
        ldi     r16, 2
        mov     r18, r24
        subi    r18, 14
button_bank_mask:
        ldi     r17, 1                  ; mask = 1 << bit
button_bank_shift:
        tst     r18
        breq    button_bank_done
        lsl     r17
        dec     r18
        rjmp    button_bank_shift
button_bank_done:
        ret

; ---------------------------------------------------------------------------
; button_state (internal) -- r16 = bank index.
; Points Z at that bank's latch byte, 0x0302 + index.
; ---------------------------------------------------------------------------
button_state:
        ldi     r30, 0x02
        ldi     r31, 0x03
        add     r30, r16
        ret

; ---------------------------------------------------------------------------
; button_delay (internal) -- about 5 ms at 16 MHz, the settling window.
; 105 * 250 * 3 cycles ~= 79k cycles. Leaves Z alone.
; ---------------------------------------------------------------------------
button_delay:
        ldi     r21, 105
button_delay_1:
        ldi     r20, 250
button_delay_2:
        dec     r20
        brne    button_delay_2
        dec     r21
        brne    button_delay_1
        ret

; ---------------------------------------------------------------------------
; button_init(uint8_t pin)
;
; Clears the direction bit (input) and sets the port bit, which on an input
; pin engages the internal pull-up rather than driving the line.
; ---------------------------------------------------------------------------
button_init:
        push    r16
        push    r17
        rcall   button_bank
        cpi     r30, 0
        breq    button_init_done
        ldd     r18, Z+1                ; DDRx &= ~mask
        mov     r19, r17
        com     r19
        and     r18, r19
        std     Z+1, r18
        ldd     r18, Z+2                ; PORTx |= mask -> pull-up on
        or      r18, r17
        std     Z+2, r18
button_init_done:
        pop     r17
        pop     r16
        ret

; ---------------------------------------------------------------------------
; uint8_t button_pressed(uint8_t pin)
;
; Returns 1 once for each press, then 0 until the button is released again.
; ---------------------------------------------------------------------------
button_pressed:
        push    r16
        push    r17
        rcall   button_bank
        cpi     r30, 0
        breq    button_pressed_zero
        ldd     r18, Z+0                ; first sample
        and     r18, r17
        brne    button_pressed_release  ; high -> open
        rcall   button_delay            ; let the contacts settle
        ldd     r18, Z+0                ; confirm
        and     r18, r17
        brne    button_pressed_release  ; bounce, not a real press

        rcall   button_state            ; genuinely down -- new press?
        ldd     r19, Z+0
        mov     r20, r19
        and     r20, r17
        brne    button_pressed_zero     ; already reported, still held
        or      r19, r17                ; latch it
        std     Z+0, r19
        ldi     r24, 1
        rjmp    button_pressed_done

button_pressed_release:
        rcall   button_state            ; clear the latch, arm the next press
        ldd     r19, Z+0
        mov     r20, r17
        com     r20
        and     r19, r20
        std     Z+0, r19
button_pressed_zero:
        ldi     r24, 0
button_pressed_done:
        ldi     r25, 0
        pop     r17
        pop     r16
        ret
