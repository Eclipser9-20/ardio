; ardio core runtime.
;
; This is ardio's own Arduino-compatible core, assembled by ardio's own
; assembler (src/avr/assembler.cpp) -- no external toolchain, no third-party
; sources.
;
; Nothing here names a particular AVR part. Every register address, the top of
; RAM and the whole pin numbering arrive as the AD_* constants and the
; __ardio_pinmap table that the device description renders ahead of assembly
; (include/ardio/avr/device.h describes the contract). Supporting another chip
; is a row in the table in src/avr/device.cpp, not a fork of this file.
;
; Calling convention matches avr-gcc:
;   * first int argument in r24:r25, second in r22:r23
;   * return value in r24:r25
;   * r1 is permanently zero
;   * r0 is scratch, destroyed by any call
;   * r18-r27, r30, r31 are call-clobbered; r2-r17, r28, r29 are preserved
;
; Every routine below sticks to call-clobbered registers only, so nothing
; needs to be pushed.
;
; On every AVR the three registers of a port sit together in data space as
; PINx, DDRx = PINx+1, PORTx = PINx+2. That is why the pin map only has to
; produce the address of PINx: the other two fall out as the displacements Z+1
; and Z+2, and the whole of the code below reaches a port through one Z.
;
; Pin numbers are runtime values, so sbi/cbi (which need a compile-time bit
; number, and an address in the low I/O space that not every part puts a port
; in) cannot be used; the bit mask is built by shifting.


; --------------------------------------------------------------- reset ---
; The reset entry point. Sits at word address 0 so it is the reset vector.
;
; SREG, SPL and SPH are the three registers whose addresses are the same on
; every AVR8 part, so they are the only ones still reached with in/out. Unlike
; every other AD_* name these three are I/O addresses rather than data-space
; ones -- 0x3F, 0x3D and 0x3E -- because in/out is the only way they are ever
; touched and in/out takes an I/O address.
__ardio_start:
        clr     r1                      ; the zero register, by convention
        out     AD_SREG, r1             ; interrupts off, flags clear
        ldi     r28, lo8(AD_RAMEND)     ; the stack grows down from the top
        out     AD_SPL, r28
        ldi     r28, hi8(AD_RAMEND)
        out     AD_SPH, r28
        call    setup
__ardio_loop:
        call    loop
        rjmp    __ardio_loop


; ------------------------------------------------------------ pin map ---
; Maps an Arduino pin number in r24 to a port.
;
;   in:  r24 = pin number
;   out: Z (r31:r30) = data address of PINx for that pin, 0 if there is no
;        such pin on this part
;        r18 = bit mask for the pin within the port, 0 in the same case
;   clobbers r0, r18, r19, r30, r31
;
; The mapping is a table in flash rather than a chain of compares, because the
; numbering is a property of the board and not of this code. __ardio_pinmap
; holds two bytes per pin -- the address of PINx, then the bit number -- so
; pin n's entry is at byte address __ardio_pinmap * 2 + n * 2, the doubling
; being because a label is a word address and lpm addresses bytes.
;
; A pin the part does not have is stored as address 0, and a caller seeing a
; zero mask must do nothing at all -- which is exactly what the out-of-range
; branch of the old compare chain did.
;
; The offset is formed as a full 16-bit quantity rather than by shifting the
; pin number in place, so a board with more than 127 digital pins cannot shift
; a bit off the top and read the wrong entry.
__ardio_pin_map:
        ldi     r18, 0                  ; assume there is no such pin
        ldi     r30, 0
        ldi     r31, 0
        cpi     r24, AD_NUM_PINS
        brsh    __ardio_pin_map_out
        mov     r19, r24
        lsl     r19                     ; r18:r19 = pin * 2
        rol     r18
        ldi     r30, lo8(__ardio_pinmap * 2)
        ldi     r31, hi8(__ardio_pinmap * 2)
        add     r30, r19
        adc     r31, r18
        lpm                             ; r0 = address of PINx
        mov     r19, r0
        adiw    r30, 1
        lpm                             ; r0 = bit number within the port
        ldi     r18, 0
        ldi     r31, 0                  ; every port register is below 0x100
        mov     r30, r19
        tst     r19
        breq    __ardio_pin_map_out     ; address 0: not a pin on this part
        ldi     r18, 1                  ; mask = 1 << bit number
__ardio_pin_shift:
        tst     r0
        breq    __ardio_pin_map_out
        lsl     r18
        dec     r0
        rjmp    __ardio_pin_shift
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
        tst     r18
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
        tst     r18
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
        tst     r18
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
;   ms in r24:r25. Busy-wait.
;
; Per outer iteration:
;   mov 1 + or 1 + breq 1 (not taken) + ldi 2 + inner (4N-1) + sbiw 2 + rjmp 2
;   = 4N + 8 cycles. One millisecond is AD_F_CPU / 1000 cycles, so
;
;       N = (AD_F_CPU / 1000 - 8) / 4
;
; which is 3998 at 16 MHz and stays comfortably above zero for any clock down
; to 1 MHz, so the inner counter can never be loaded with 0 and wrap to 256.
delay:
        mov     r20, r24
        or      r20, r25
        breq    delay_done              ; delay(0) returns immediately
delay_outer:
        ldi     r26, lo8((AD_F_CPU / 1000 - 8) / 4)
        ldi     r27, hi8((AD_F_CPU / 1000 - 8) / 4)
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
;
; This one is deliberately NOT derived from AD_F_CPU. The fixed overhead of the
; outer loop is already 7 cycles, so below about 12 MHz there is no inner count
; that makes the body come out at one microsecond -- a formula would silently
; produce a delay that is too long rather than a wrong one, but it would also
; read as if it were exact. A slower part needs this loop restructured, not
; rescaled, so the 16 MHz calibration is left visible until then.
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


; ARDIO_FALLBACK_BEGIN
; -------------------------------------------------------------- sketch ---
; Fallback definitions so this file assembles on its own. Everything below the
; ARDIO_FALLBACK_BEGIN marker is dropped when this file is linked against a
; sketch, which supplies its own setup and loop.
setup:
        ret
loop:
        ret
