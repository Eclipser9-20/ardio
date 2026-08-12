; ardio core runtime for the ATmega328P at 16 MHz.
;
; This is ardio's own Arduino-compatible core, written straight from the
; ATmega328P datasheet register map and assembled by ardio's own assembler
; (src/avr/assembler.cpp) -- no external toolchain, no third-party sources.
;
; Calling convention matches avr-gcc:
;   * first int argument in r24:r25, second in r22:r23
;   * return value in r24:r25
;   * r1 is permanently zero
;   * r18-r27, r30, r31 are call-clobbered; r2-r17, r28, r29 are preserved
;
; Every routine below sticks to call-clobbered registers only, so nothing
; needs to be pushed.
;
; Data-space addresses of the port registers (datasheet register summary):
;
;       PINB 0x23   DDRB 0x24   PORTB 0x25
;       PINC 0x26   DDRC 0x27   PORTC 0x28
;       PIND 0x29   DDRD 0x2A   PORTD 0x2B
;
; Each group is PINx, PINx+1 = DDRx, PINx+2 = PORTx, which is why the pin
; mapper only needs to produce the PINx address: the other two fall out as
; displacements Z+1 and Z+2.
;
; Pin numbers are runtime values, so sbi/cbi (which need a compile-time bit
; number) cannot be used; the bit mask is built by shifting and the register
; is reached through Z with ldd/std.


; --------------------------------------------------------------- reset ---
; The reset entry point. Sits at word address 0 so it is the reset vector.
__ardio_start:
        clr     r1                      ; the zero register, by convention
        out     0x3F, r1                ; SREG = 0: interrupts off, flags clear
        ldi     r28, 0xFF               ; RAMEND = 0x08FF on the ATmega328P
        out     0x3D, r28               ; SPL
        ldi     r28, 0x08
        out     0x3E, r28               ; SPH
        call    setup
__ardio_loop:
        call    loop
        rjmp    __ardio_loop


; ------------------------------------------------------------ pin map ---
; Maps an Arduino pin number in r24 to a port.
;
;   in:  r24 = pin number
;   out: r23 = 1 if the pin exists, 0 if not
;        Z (r31:r30) = address of PINx for that port
;        r18 = bit mask for the pin within the port
;   clobbers r18, r19, r23, r30, r31
;
;   pins  0-7  -> PORTD bits 0-7
;   pins  8-13 -> PORTB bits 0-5
;   pins 14-19 -> PORTC bits 0-5   (A0-A5)
__ardio_pin_map:
        ldi     r23, 0                  ; assume the pin is out of range
        cpi     r24, 20
        brsh    __ardio_pin_map_out
        mov     r19, r24                ; r19 becomes the bit index
        ldi     r31, 0                  ; all port registers live below 0x100
        ldi     r30, 0x29               ; PIND
        cpi     r24, 8
        brlo    __ardio_pin_mask
        ldi     r30, 0x23               ; PINB
        subi    r19, 8
        cpi     r24, 14
        brlo    __ardio_pin_mask
        ldi     r30, 0x26               ; PINC
        subi    r19, 6                  ; 8 already taken off above -> 14
__ardio_pin_mask:
        ldi     r18, 1                  ; mask = 1 << bit index
__ardio_pin_shift:
        tst     r19
        breq    __ardio_pin_ready
        lsl     r18
        dec     r19
        rjmp    __ardio_pin_shift
__ardio_pin_ready:
        ldi     r23, 1
__ardio_pin_map_out:
        ret


; ----------------------------------------------------------- pinMode ---
; void pinMode(uint8_t pin, uint8_t mode)
;   pin  in r24, mode in r22
;   mode 0 = INPUT, 1 = OUTPUT, 2 = INPUT_PULLUP
;
; INPUT clears the direction bit and the pull-up; INPUT_PULLUP clears the
; direction bit and sets the pull-up; OUTPUT sets the direction bit and
; leaves PORTx alone (the output latch keeps whatever was last written).
pinMode:
        rcall   __ardio_pin_map
        tst     r23
        breq    pinMode_done
        ldd     r21, Z+1                ; DDRx
        cpi     r22, 1
        breq    pinMode_output
        mov     r20, r18
        com     r20                     ; ~mask
        and     r21, r20
        std     Z+1, r21                ; DDRx &= ~mask  -- input
        ldd     r21, Z+2                ; PORTx
        cpi     r22, 2
        breq    pinMode_pullup
        and     r21, r20                ; PORTx &= ~mask -- no pull-up
        rjmp    pinMode_store
pinMode_pullup:
        or      r21, r18                ; PORTx |= mask  -- pull-up on
pinMode_store:
        std     Z+2, r21
        ret
pinMode_output:
        or      r21, r18                ; DDRx |= mask
        std     Z+1, r21
pinMode_done:
        ret


; ------------------------------------------------------ digitalWrite ---
; void digitalWrite(uint8_t pin, uint8_t value)
;   pin in r24, value in r22 (0 = LOW, anything else = HIGH)
;
; On an input pin this toggles the pull-up, which is the same behaviour the
; hardware gives PORTx either way.
digitalWrite:
        rcall   __ardio_pin_map
        tst     r23
        breq    digitalWrite_done
        ldd     r21, Z+2                ; PORTx
        tst     r22
        breq    digitalWrite_low
        or      r21, r18                ; PORTx |= mask
        rjmp    digitalWrite_store
digitalWrite_low:
        mov     r20, r18
        com     r20
        and     r21, r20                ; PORTx &= ~mask
digitalWrite_store:
        std     Z+2, r21
digitalWrite_done:
        ret


; ------------------------------------------------------- digitalRead ---
; uint8_t digitalRead(uint8_t pin)
;   pin in r24, returns 0 or 1 in r24 (r25 cleared for an int-widened result)
digitalRead:
        rcall   __ardio_pin_map
        ldi     r25, 0
        tst     r23
        breq    digitalRead_low
        ldd     r20, Z+0                ; PINx
        and     r20, r18
        breq    digitalRead_low
        ldi     r24, 1
        ret
digitalRead_low:
        ldi     r24, 0
        ret


; ------------------------------------------------------------- delay ---
; void delay(uint16_t ms)
;   ms in r24:r25. Busy-wait, calibrated for 16 MHz (16000 cycles per ms).
;
; Per outer iteration:
;   mov 1 + or 1 + breq 1 (not taken) + ldi 2 + inner (4N-1) + sbiw 2 + rjmp 2
;   = 4N + 8 cycles. N = 3998 gives 16000 cycles = 1.000 ms.
delay:
        mov     r20, r24
        or      r20, r25
        breq    delay_done              ; delay(0) returns immediately
delay_outer:
        ldi     r26, 0x9E               ; 3998
        ldi     r27, 0x0F
delay_inner:
        sbiw    r26, 1
        brne    delay_inner
        sbiw    r24, 1
        mov     r20, r24
        or      r20, r25
        brne    delay_outer
delay_done:
        ret


; --------------------------------------------------- delayMicroseconds ---
; void delayMicroseconds(uint16_t us)
;   us in r24:r25. One microsecond is 16 cycles at 16 MHz.
;
; Per outer iteration:
;   ldi 2 + inner (4*2 - 1 = 7) + sbiw 2 + mov 1 + or 1 + brne 2 = 15,
;   plus the 1-cycle fall-through on the final pass. Close enough that the
;   error stays under a cycle per microsecond; call overhead makes very short
;   requests slightly long, exactly as on other AVR cores.
delayMicroseconds:
        mov     r20, r24
        or      r20, r25
        breq    delayMicroseconds_done
delayMicroseconds_outer:
        ldi     r26, 2
        ldi     r27, 0
delayMicroseconds_inner:
        sbiw    r26, 1
        brne    delayMicroseconds_inner
        sbiw    r24, 1
        mov     r20, r24
        or      r20, r25
        brne    delayMicroseconds_outer
delayMicroseconds_done:
        ret


; -------------------------------------------------------------- sketch ---
; Fallback definitions so this file assembles and links on its own. A sketch
; provides its own setup/loop; when it does, these two are replaced.
setup:
        ret
loop:
        ret
