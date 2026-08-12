; ardio stepper runtime -- 28BYJ-48 through a ULN2003, ATmega328P at 16 MHz.
;
; Written from the motor and driver datasheets. The ULN2003 is just seven
; Darlington sinks, so each of the four MCU pins simply grounds one coil of the
; 28BYJ-48's unipolar winding when driven high; there is no direction pin and
; no enable, the pattern on the four pins *is* the rotor position.
;
; Drive mode: 4-step FULL-DRIVE (two coils energised at a time). Full drive is
; chosen over 8-step half drive because it gives roughly double the holding
; torque for the same current, which is what a label mechanism dragging tape
; needs; the cost is half the angular resolution. The sequence, with bit 0 =
; pin1 ... bit 3 = pin4, is
;
;     phase 0: 1100    phase 1: 0110    phase 2: 0011    phase 3: 1001
;
; i.e. the energised pair walks one coil at a time, and each pattern overlaps
; its neighbour by one coil so the rotor is always pulled, never released.
; Running the phase index downwards instead of upwards reverses rotation.
;
; Steps per revolution: the 28BYJ-48 is a 4-step-per-electrical-cycle motor
; with 8 electrical cycles per internal turn and a 64:1 reduction gearbox, so
; one output shaft revolution is 2048 full steps. Hence
;
;     step_delay_us = 60e6 / (rpm * 2048) = 29296 / rpm
;
; which stepper_set_speed computes with a 16-bit shift-and-subtract divide.
; rpm 0 is treated as 1, and the delay is clamped to at least 2000 us because
; a 28BYJ-48 will not reliably follow steps faster than about 15 rpm.
;
; SRAM state (one stepper instance; this assembler has only absolute lds/sts,
; no indexed addressing, so the addresses are fixed). Chosen at 0x0340, above
; the 0x0200 floor, clear of core.S, and clear of servo.S's 0x0300 block:
;
;     0x0340  coil pin 1
;     0x0341  coil pin 2
;     0x0342  coil pin 3
;     0x0343  coil pin 4
;     0x0344  current phase index (0..3)
;     0x0345  step delay, low byte  (microseconds)
;     0x0346  step delay, high byte
;     0x0347  direction of the run in progress (1 forward, 0 reverse)
;     0x0348  coil pattern of the phase being applied
;
; ABI: avr-gcc's. Args in r24:r25, r22:r23, r20:r21, r18:r19; return r24:r25;
; r1 is always zero; r18-r27, r30, r31 are call-clobbered.

; ---------------------------------------------------------------------------
; stepper_init(uint8_t pin1, uint8_t pin2, uint8_t pin3, uint8_t pin4)
;   pin1 r24, pin2 r22, pin3 r20, pin4 r18
;
; Records the four coil pins, makes them outputs, de-energises every coil and
; parks at phase 0 with a default speed of 10 rpm.
; ---------------------------------------------------------------------------
stepper_init:
        sts     0x0340, r24
        sts     0x0341, r22
        sts     0x0342, r20
        sts     0x0343, r18
        clr     r18
        sts     0x0344, r18

        ; Drive all four low, which both sets DDR and clears PORT.
        lds     r24, 0x0340
        rcall   coil_off
        lds     r24, 0x0341
        rcall   coil_off
        lds     r24, 0x0342
        rcall   coil_off
        lds     r24, 0x0343
        rcall   coil_off

        ldi     r24, 10                 ; default 10 rpm
        ldi     r25, 0
        rcall   stepper_set_speed
        ret

; ---------------------------------------------------------------------------
; stepper_set_speed(uint16_t rpm)  -- rpm in r24:r25
;
; Stores 29296 / rpm microseconds as the per-step delay, floored at 2000 us.
; ---------------------------------------------------------------------------
stepper_set_speed:
        sbiw    r24, 0
        brne    ss_have_rpm
        ldi     r24, 1                  ; rpm 0 would divide by zero
        ldi     r25, 0
ss_have_rpm:
        movw    r20, r24                ; divisor = rpm
        ldi     r26, 0x70               ; dividend = 29296
        ldi     r27, 0x72
        rcall   stp_div16

        ldi     r30, 0xD0               ; clamp to a floor of 2000 us
        ldi     r31, 0x07
        cp      r26, r30
        cpc     r27, r31
        brsh    ss_store
        movw    r26, r30
ss_store:
        sts     0x0345, r26
        sts     0x0346, r27
        ret

; ---------------------------------------------------------------------------
; stepper_step(int16_t steps)  -- steps in r24:r25
;
; Turns |steps| full steps, forwards for a positive count and in reverse for a
; negative one, waiting the configured delay between each. Blocks until done,
; and leaves the last pattern energised so the motor holds position.
; ---------------------------------------------------------------------------
stepper_step:
        push    r28                     ; r28:r29 is call-saved, and it is the
        push    r29                     ; only pair the helpers never touch
        ldi     r18, 1                  ; direction: 1 forward, 0 reverse
        sbrs    r25, 7
        rjmp    st_positive
        ldi     r18, 0
        com     r25                     ; negate r24:r25
        neg     r24
        sbci    r25, 0xFF
st_positive:
        sts     0x0347, r18             ; every register is clobbered by the
                                        ; helpers, so direction lives in SRAM
        movw    r28, r24                ; remaining steps
st_loop:
        sbiw    r28, 0
        breq    st_done

        ; ---- advance the phase index, wrapping in 0..3 ----------------------
        lds     r24, 0x0344
        lds     r18, 0x0347
        tst     r18
        breq    st_back
        subi    r24, 0xFF               ; += 1
        rjmp    st_wrap
st_back:
        subi    r24, 1                  ; -= 1
st_wrap:
        andi    r24, 3
        sts     0x0344, r24

        rcall   apply_phase

        lds     r24, 0x0345             ; wait out the step time
        lds     r25, 0x0346
        rcall   stp_delay_us

        sbiw    r28, 1
        rjmp    st_loop
st_done:
        pop     r29
        pop     r28
        ret

; ---------------------------------------------------------------------------
; apply_phase: drive the four coil pins from the phase index at 0x0344.
;
; Clobbers r18-r27, r30, r31.
; ---------------------------------------------------------------------------
apply_phase:
        lds     r30, 0x0344
        ldi     r31, 0b1100             ; phase 0
        cpi     r30, 1
        brne    ap_try2
        ldi     r31, 0b0110
        rjmp    ap_drive
ap_try2:
        cpi     r30, 2
        brne    ap_try3
        ldi     r31, 0b0011
        rjmp    ap_drive
ap_try3:
        cpi     r30, 3
        brne    ap_drive
        ldi     r31, 0b1001

ap_drive:
        sts     0x0348, r31             ; the coil helpers clobber every
                                        ; scratch register, so the pattern is
                                        ; re-read from SRAM for each coil
        lds     r24, 0x0340
        lds     r18, 0x0348
        sbrc    r18, 0
        rjmp    ap_c1_on
        rcall   coil_off
        rjmp    ap_c2
ap_c1_on:
        rcall   coil_on
ap_c2:
        lds     r24, 0x0341
        lds     r18, 0x0348
        sbrc    r18, 1
        rjmp    ap_c2_on
        rcall   coil_off
        rjmp    ap_c3
ap_c2_on:
        rcall   coil_on
ap_c3:
        lds     r24, 0x0342
        lds     r18, 0x0348
        sbrc    r18, 2
        rjmp    ap_c3_on
        rcall   coil_off
        rjmp    ap_c4
ap_c3_on:
        rcall   coil_on
ap_c4:
        lds     r24, 0x0343
        lds     r18, 0x0348
        sbrc    r18, 3
        rjmp    ap_c4_on
        rcall   coil_off
        ret
ap_c4_on:
        rcall   coil_on
        ret

; ---------------------------------------------------------------------------
; coil_on / coil_off: energise or release the coil on the pin in r24.
; Both make the pin an output first, so stepper_init needs no separate pass.
; ---------------------------------------------------------------------------
coil_on:
        rcall   stp_pin_decode
        rcall   stp_pin_ddr_out
        rcall   stp_pin_high
        ret
coil_off:
        rcall   stp_pin_decode
        rcall   stp_pin_ddr_out
        rcall   stp_pin_low
        ret

; ---------------------------------------------------------------------------
; stp_div16: r26:r27 = r26:r27 / r20:r21   (remainder left in r18:r19)
;
; Restoring shift-and-subtract, 16 iterations, MSB first: the dividend shifts
; out of the top of r26:r27 while the quotient shifts in at the bottom.
; ---------------------------------------------------------------------------
stp_div16:
        clr     r18
        clr     r19
        ldi     r22, 16
stp_div16_loop:
        lsl     r26
        rol     r27
        rol     r18
        rol     r19
        cp      r18, r20
        cpc     r19, r21
        brlo    stp_div16_next
        sub     r18, r20
        sbc     r19, r21
        ori     r26, 1
stp_div16_next:
        dec     r22
        brne    stp_div16_loop
        ret

; ---------------------------------------------------------------------------
; stp_delay_us: busy-wait r24:r25 microseconds at 16 MHz.
; The loop body is exactly 16 cycles: 12 nop + sbiw + brne taken.
; ---------------------------------------------------------------------------
stp_delay_us:
        sbiw    r24, 0
        breq    stp_du_done
stp_du_loop:
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
        brne    stp_du_loop
stp_du_done:
        ret

; ---------------------------------------------------------------------------
; stp_pin_decode: r24 = Arduino pin number -> r20 = port id, r21 = bit mask.
; Port ids: 0 = PORTD (pins 0-7), 1 = PORTB (8-13), 2 = PORTC (14-19).
; Clobbers r22, r23.
; ---------------------------------------------------------------------------
stp_pin_decode:
        mov     r22, r24
        cpi     r24, 8
        brlo    stp_pd_portd
        cpi     r24, 14
        brlo    stp_pd_portb
        ldi     r20, 2
        subi    r22, 14
        rjmp    stp_pd_mask
stp_pd_portb:
        ldi     r20, 1
        subi    r22, 8
        rjmp    stp_pd_mask
stp_pd_portd:
        ldi     r20, 0
stp_pd_mask:
        ldi     r21, 1
stp_pd_shift:
        tst     r22
        breq    stp_pd_done
        lsl     r21
        dec     r22
        rjmp    stp_pd_shift
stp_pd_done:
        ret

; ---------------------------------------------------------------------------
; stp_pin_ddr_out / stp_pin_high / stp_pin_low: act on port id r20, mask r21.
; sbi/cbi need a compile-time address, so the port is picked by branch.
; Clobbers r19, r23.
; ---------------------------------------------------------------------------
stp_pin_ddr_out:
        cpi     r20, 1
        breq    stp_pdo_b
        cpi     r20, 2
        breq    stp_pdo_c
        in      r23, 0x0A               ; DDRD
        or      r23, r21
        out     0x0A, r23
        ret
stp_pdo_b:
        in      r23, 0x04               ; DDRB
        or      r23, r21
        out     0x04, r23
        ret
stp_pdo_c:
        in      r23, 0x07               ; DDRC
        or      r23, r21
        out     0x07, r23
        ret

stp_pin_high:
        cpi     r20, 1
        breq    stp_ph_b
        cpi     r20, 2
        breq    stp_ph_c
        in      r23, 0x0B               ; PORTD
        or      r23, r21
        out     0x0B, r23
        ret
stp_ph_b:
        in      r23, 0x05               ; PORTB
        or      r23, r21
        out     0x05, r23
        ret
stp_ph_c:
        in      r23, 0x08               ; PORTC
        or      r23, r21
        out     0x08, r23
        ret

stp_pin_low:
        mov     r19, r21
        com     r19
        cpi     r20, 1
        breq    stp_pl_b
        cpi     r20, 2
        breq    stp_pl_c
        in      r23, 0x0B               ; PORTD
        and     r23, r19
        out     0x0B, r23
        ret
stp_pl_b:
        in      r23, 0x05               ; PORTB
        and     r23, r19
        out     0x05, r23
        ret
stp_pl_c:
        in      r23, 0x08               ; PORTC
        and     r23, r19
        out     0x08, r23
        ret
