; ardio servo runtime -- ATmega328P at 16 MHz.
;
; Written from the hobby-servo signal spec rather than from any existing
; library: a servo expects a positive pulse repeated at about 50 Hz, where the
; pulse width, not its period, carries the commanded angle. 1000 us is one end
; of travel, 2000 us the other, so
;
;     width_us = 1000 + angle * 1000 / 180        (angle in 0..180)
;
; Approach: the pulse is bit-banged in a timed loop rather than driven from
; Timer1. Timer1 would hold the output on its own and free the CPU, but it can
; only serve OC1A/OC1B, and the Label Maker's servo is on an arbitrary pin. A
; timed loop works on any of the 20 I/O pins and needs no interrupt vector, at
; the cost of blocking for the length of the pulse -- at most 2 ms, which is
; well under the 20 ms frame, so a caller that calls servo_write in its main
; loop naturally produces a valid 50 Hz signal. servo_write emits exactly one
; pulse per call; call it about every 20 ms to hold a position.
;
; Timing base: 16 MHz means 16 cycles per microsecond. delay_us below burns
; 16 cycles per iteration (12 nop + sbiw + brne), so its argument is
; microseconds directly. Call and setup overhead adds well under 1 us, far
; inside a servo's own tolerance.
;
; SRAM state (one servo -- the Label Maker has exactly one). Fixed addresses
; are used because this assembler has no indexed load/store, only absolute
; lds/sts. Chosen at 0x0300, above the 0x0200 floor and clear of core.S:
;
;     0x0300  attached flag (0 or 1)
;     0x0301  attached pin number
;     0x0302  last commanded angle
;
; ABI: avr-gcc's. First int arg r24:r25, second r22:r23; r1 is always zero;
; r18-r27, r30, r31 are call-clobbered.

; ---------------------------------------------------------------------------
; servo_attach(uint8_t pin)  -- pin in r24
;
; Marks the pin as this runtime's servo output, drives it low and parks the
; recorded angle at the 90 degree centre. No pulse is emitted yet.
; ---------------------------------------------------------------------------
servo_attach:
        sts     0x0301, r24
        ldi     r18, 1
        sts     0x0300, r18
        ldi     r18, 90
        sts     0x0302, r18
        rcall   pin_decode
        rcall   pin_ddr_out
        rcall   pin_low
        ret

; ---------------------------------------------------------------------------
; servo_write(uint8_t pin, uint8_t angle)  -- pin r24, angle r22
;
; Records the angle and emits one pulse of the matching width on the pin.
; Angles above 180 are clamped. Does nothing if nothing is attached.
; ---------------------------------------------------------------------------
servo_write:
        lds     r18, 0x0300
        tst     r18
        breq    sw_return
        sts     0x0301, r24
        mov     r18, r22
        cpi     r18, 181
        brlo    sw_clamped
        ldi     r18, 180
sw_clamped:
        sts     0x0302, r18

        ; ---- width_us = 1000 + angle * 50 / 9 -------------------------------
        ; angle * 1000 / 180 reduces to angle * 50 / 9, which keeps the
        ; intermediate under 9000 and so inside 16 bits.
        ;
        ; 50 = 2 + 16 + 32, so three shifted copies added together.
        mov     r26, r18
        clr     r27
        lsl     r26                     ; r26:r27 = angle * 2
        rol     r27
        movw    r30, r26                ; keep the *2 term
        lsl     r26                     ; *4
        rol     r27
        lsl     r26                     ; *8
        rol     r27
        lsl     r26                     ; *16
        rol     r27
        add     r30, r26                ; *2 + *16
        adc     r31, r27
        lsl     r26                     ; *32
        rol     r27
        add     r26, r30                ; *32 + *18 = *50
        adc     r27, r31

        ldi     r20, 9                  ; divide by 9
        ldi     r21, 0
        rcall   div16

        ldi     r30, 0xE8               ; + 1000 us of zero-degree offset
        ldi     r31, 0x03
        add     r26, r30
        adc     r27, r31

        ; ---- emit the pulse -------------------------------------------------
        rcall   pin_decode              ; pin is still in r24
        rcall   pin_ddr_out
        rcall   pin_high
        movw    r24, r26
        rcall   delay_us
        rcall   pin_low
sw_return:
        ret

; ---------------------------------------------------------------------------
; servo_detach(uint8_t pin)  -- pin in r24
;
; Stops the runtime pulsing and leaves the pin low, so the servo goes limp
; rather than holding torque against whatever the mechanism does next.
; ---------------------------------------------------------------------------
servo_detach:
        clr     r18
        sts     0x0300, r18
        rcall   pin_decode
        rcall   pin_low
        ret

; ---------------------------------------------------------------------------
; div16: r26:r27 = r26:r27 / r20:r21   (remainder left in r18:r19)
;
; Restoring shift-and-subtract, 16 iterations, MSB first. The dividend is
; shifted out of the top of r26:r27 while the quotient shifts in at the
; bottom, so no third register pair is needed.
; ---------------------------------------------------------------------------
div16:
        clr     r18
        clr     r19
        ldi     r22, 16
div16_loop:
        lsl     r26                     ; shift dividend into the remainder
        rol     r27
        rol     r18
        rol     r19
        cp      r18, r20
        cpc     r19, r21
        brlo    div16_next              ; remainder < divisor: quotient bit 0
        sub     r18, r20
        sbc     r19, r21
        ori     r26, 1                  ; quotient bit 1
div16_next:
        dec     r22
        brne    div16_loop
        ret

; ---------------------------------------------------------------------------
; delay_us: busy-wait r24:r25 microseconds at 16 MHz.
;
; The loop body is exactly 16 cycles: 12 nop (12) + sbiw (2) + brne taken (2).
; ---------------------------------------------------------------------------
delay_us:
        sbiw    r24, 0
        breq    du_done
du_loop:
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        sbiw    r24, 1
        brne    du_loop
du_done:
        ret

; ---------------------------------------------------------------------------
; pin_decode: r24 = Arduino pin number -> r20 = port id, r21 = bit mask.
;
; Port ids: 0 = PORTD (pins 0-7), 1 = PORTB (8-13), 2 = PORTC (14-19).
; Clobbers r22, r23.
; ---------------------------------------------------------------------------
pin_decode:
        mov     r22, r24
        cpi     r24, 8
        brlo    pd_portd
        cpi     r24, 14
        brlo    pd_portb
        ldi     r20, 2
        subi    r22, 14
        rjmp    pd_mask
pd_portb:
        ldi     r20, 1
        subi    r22, 8
        rjmp    pd_mask
pd_portd:
        ldi     r20, 0
pd_mask:
        ldi     r21, 1
pd_shift:
        tst     r22
        breq    pd_done
        lsl     r21
        dec     r22
        rjmp    pd_shift
pd_done:
        ret

; ---------------------------------------------------------------------------
; pin_ddr_out / pin_high / pin_low: act on port id r20, mask r21.
;
; sbi/cbi need a compile-time address, so the port is selected by branch
; rather than computed. Clobbers r19, r23.
; ---------------------------------------------------------------------------
pin_ddr_out:
        cpi     r20, 1
        breq    pdo_b
        cpi     r20, 2
        breq    pdo_c
        in      r23, 0x0A               ; DDRD
        or      r23, r21
        out     0x0A, r23
        ret
pdo_b:
        in      r23, 0x04               ; DDRB
        or      r23, r21
        out     0x04, r23
        ret
pdo_c:
        in      r23, 0x07               ; DDRC
        or      r23, r21
        out     0x07, r23
        ret

pin_high:
        cpi     r20, 1
        breq    ph_b
        cpi     r20, 2
        breq    ph_c
        in      r23, 0x0B               ; PORTD
        or      r23, r21
        out     0x0B, r23
        ret
ph_b:
        in      r23, 0x05               ; PORTB
        or      r23, r21
        out     0x05, r23
        ret
ph_c:
        in      r23, 0x08               ; PORTC
        or      r23, r21
        out     0x08, r23
        ret

pin_low:
        mov     r19, r21
        com     r19
        cpi     r20, 1
        breq    pl_b
        cpi     r20, 2
        breq    pl_c
        in      r23, 0x0B               ; PORTD
        and     r23, r19
        out     0x0B, r23
        ret
pl_b:
        in      r23, 0x05               ; PORTB
        and     r23, r19
        out     0x05, r23
        ret
pl_c:
        in      r23, 0x08               ; PORTC
        and     r23, r19
        out     0x08, r23
        ret
