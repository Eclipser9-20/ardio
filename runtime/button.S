; ardio runtime -- debounced push-button input, active low.
;
; Buttons are wired the usual way: one side to the pin, one side to ground,
; with the AVR's internal pull-up holding the pin high while the button is
; open. So "pressed" means the pin reads 0.
;
; Debouncing here is confirm-by-resample plus edge latching: the pin must read
; low, still read low ~5 ms later, and not already have been reported as down.
; That rejects contact bounce (which settles well inside a millisecond on these
; tactile switches) and makes button_pressed() fire exactly once per physical
; press no matter how often it is polled.
;
; Nothing here knows the board's numbering. A pin becomes a port register and a
; bit through __ardio_pinmap, the flash table the device description renders
; alongside the runtime. Each port's three registers are consecutive in data
; space (PINx, DDRx = PINx+1, PORTx = PINx+2) on every AVR, so one Z pointer at
; PINx reaches all three through the displacement forms Z+0, Z+1 and Z+2.
;
; SRAM state (documented so other runtime files do not collide):
;     0x0308-0x0317  the press latch, one bit per pin
;
; The latch used to be three bytes, one per port, indexed by which of PORTD,
; PORTB or PORTC the pin belonged to. There is no fixed set of ports any more,
; so it is now indexed by the pin number itself: pin n's bit is bit n & 7 of
; the byte at 0x0308 + n / 8. Sixteen bytes cover 128 pins, and a pin numbered
; at or above that is refused outright by button_bank rather than allowed to
; write past the end of the region.
; (lcd.S owns 0x0300-0x0301. servo.S owns 0x0300-0x0302. wire.S owns nothing.)
;
; Registers: avr-gcc convention. r0 is scratch and is destroyed here. r16 and
; r17 are call-saved and are pushed by both public entry points.

.equ    BUTTON_LATCH,   0x0308

; ---------------------------------------------------------------------------
; button_bank (internal) -- r24 = pin number.
;
; Returns Z = data address of that pin's PINx register and r17 = the bit mask
; within that port. A pin the part does not have, or one numbered past the
; latch region, comes back with Z = 0 and r17 = 0, which both callers treat as
; "do nothing".
;
; Clobbers r0, r17, r18, r19, r30, r31. The pin number in r24 survives, which
; is what lets button_state be called afterwards.
;
; Pin n's entry is at byte address __ardio_pinmap * 2 + n * 2 -- doubled
; because a label names a word while lpm addresses bytes. The offset is formed
; as a full 16-bit value rather than by shifting the pin number where it sits,
; so a high pin number cannot lose its top bit and select a different pin.
; ---------------------------------------------------------------------------
button_bank:
        ldi     r17, 0                  ; assume there is no such pin
        ldi     r30, 0
        ldi     r31, 0
        cpi     r24, AD_NUM_PINS
        brsh    button_bank_done
        cpi     r24, 128                ; past the latch region -- see above
        brsh    button_bank_done
        mov     r18, r24
        ldi     r19, 0
        lsl     r18                     ; r19:r18 = pin * 2
        rol     r19
        ldi     r30, lo8(__ardio_pinmap * 2)
        ldi     r31, hi8(__ardio_pinmap * 2)
        add     r30, r18
        adc     r31, r19
        lpm                             ; r0 = address of PINx
        mov     r18, r0
        adiw    r30, 1
        lpm                             ; r0 = bit number within the port
        ldi     r31, 0                  ; every port register is below 0x100
        mov     r30, r18
        tst     r18
        breq    button_bank_done        ; address 0: not a pin on this part
        ldi     r17, 1                  ; mask = 1 << bit number
button_bank_shift:
        tst     r0
        breq    button_bank_done
        lsl     r17
        dec     r0
        rjmp    button_bank_shift
button_bank_done:
        ret

; ---------------------------------------------------------------------------
; button_state (internal) -- r24 = pin number.
;
; Points Z at that pin's latch byte and puts its bit mask in r16. Clobbers
; r16, r18, r19, r30, r31, and is only ever called after button_bank has
; already accepted the pin, so the number is known to be under 128.
; ---------------------------------------------------------------------------
button_state:
        mov     r18, r24
        lsr     r18
        lsr     r18
        lsr     r18                     ; pin / 8 selects the byte
        ldi     r19, 0
        ldi     r30, lo8(BUTTON_LATCH)
        ldi     r31, hi8(BUTTON_LATCH)
        add     r30, r18
        adc     r31, r19
        mov     r19, r24
        andi    r19, 7                  ; pin % 8 selects the bit
        ldi     r16, 1
button_state_shift:
        tst     r19
        breq    button_state_done
        lsl     r16
        dec     r19
        rjmp    button_state_shift
button_state_done:
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
        and     r20, r16
        brne    button_pressed_zero     ; already reported, still held
        or      r19, r16                ; latch it
        std     Z+0, r19
        ldi     r24, 1
        rjmp    button_pressed_done

button_pressed_release:
        rcall   button_state            ; clear the latch, arm the next press
        ldd     r19, Z+0
        mov     r20, r16
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
