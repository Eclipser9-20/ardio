; ardio infrared receive runtime, at 16 MHz.
;
; Written from the published NEC remote-control protocol timings rather than
; from any existing library. Assembled by ardio's own assembler
; (src/avr/assembler.cpp): no directives, no external toolchain.
;
; ---------------------------------------------------------------- protocol ---
;
; A TSOP-style receiver module demodulates the 38 kHz carrier and presents a
; plain logic level. It is ACTIVE LOW: the line idles high and a burst of
; carrier ("a mark") pulls it low. A gap ("a space") lets it float back high,
; which is why the pin is configured as an input with the internal pull-up.
;
; A NEC frame is:
;
;       9 ms mark, 4.5 ms space              leader
;       32 bits, LSB first                   address, ~address, cmd, ~cmd
;       562.5 us mark                        stop
;
; Each bit is a 562.5 us mark followed by a space whose length carries the
; value: 562.5 us for a 0, 1687.5 us for a 1. A held key sends a repeat frame
; instead -- a 9 ms mark and a 2.25 ms space with no data after it.
;
; ----------------------------------------------------------- the approach ---
;
; Polling with cycle-counted timing loops, not interrupts. ir_decode blocks
; until a frame arrives or its timeout expires, which suits a sketch that
; checks the remote once per pass of loop(). It costs nothing when idle beyond
; the call itself, needs no interrupt vector, and works on any I/O pin the
; board has -- an external-interrupt or pin-change scheme would either be tied
; to particular pins or have to demultiplex the whole port.
;
; Every pulse is measured by counting ticks of one sampling loop. That loop is
; exactly 801 cycles = 50.06 us at 16 MHz (see ir_measure_mark for the count),
; so the protocol's pulses become these tick counts:
;
;       9 ms mark      180        562.5 us    11.2
;       4.5 ms space    90        1687.5 us   33.7
;       2.25 ms space   45
;
; TOLERANCE: +/-25%, the usual figure for NEC decoders, widened by one tick
; either way to absorb the quantisation of a 50 us sampling period. Accepted
; windows, in ticks:
;
;       leader mark    135..225      bit mark        7..15
;       leader space    67..112      bit space "0"   7..15
;       repeat space    34..56       bit space "1"  24..44
;
; A pulse still measuring at 250 ticks (12.5 ms) is treated as a timeout,
; which is past the longest legal pulse (the 9 ms leader) with room to spare.
; The wait for a frame to begin gives up after 2000 ticks, about 100 ms.
;
; Only the command byte is checked against its inverse. The address inverse is
; deliberately not checked: extended NEC uses that byte as the high half of a
; 16-bit address, and rejecting it would lock out those remotes.
;
; ------------------------------------------------------------ SRAM layout ---
;
; Fixed addresses, since this assembler has absolute lds/sts only. Placed at
; 0x0380, clear of every other runtime file (servo.S at 0x0300, stepper.S at
; 0x0340, lcd.S at 0x0300-0x0301 and button.S at 0x0308-0x0317) and inside the
; SRAM of every part in the device table:
;
;       0x0380  available flag (1 once a frame has been decoded)
;       0x0381  receive pin number
;       0x0382  last decoded address byte
;       0x0383  last decoded command byte
;       0x0384  configured flag (1 once ir_begin has run)
;
; -------------------------------------------------------------------- ABI ---
;
; avr-gcc's: first int argument in r24:r25, return value in r24:r25, r1 is
; permanently zero, r0 is scratch, r18-r27 and r30:r31 are call-clobbered, and
; r2-r17 and r28:r29 are preserved. Nothing here touches a call-saved register,
; so no routine needs to push anything.
;
; Register roles inside a decode:
;
;       Z (r31:r30)  address of PINx for the receive pin
;       r18          bit mask of the pin within that port
;       r19          scratch
;       r20          tick count returned by a measurement
;       r21          sampling-loop delay counter
;       r22..r25     the 32 bits as they shift in: address, ~address, cmd, ~cmd
;       r26          bit counter / long-timeout counter
;
; Part of ardio. Licensed under the GNU General Public License v3.


; ---------------------------------------------------------------------------
; ir_begin(uint8_t pin) -- pin in r24
;
; Records the pin and configures it as an input with the pull-up engaged. The
; pull-up matters: an idle receiver module drives its output high, but the line
; must not float during the brief window before the module powers up.
; ---------------------------------------------------------------------------
ir_begin:
        sts     0x0381, r24             ; remember the pin
        ldi     r19, 0
        sts     0x0380, r19             ; nothing decoded yet
        ldi     r19, 1
        sts     0x0384, r19             ; configured
        rcall   ir_pin_setup
        tst     r18                     ; mask 0 means the pin does not exist
        breq    ir_begin_done
        ldd     r19, Z+1                ; DDRx
        mov     r20, r18
        com     r20
        and     r19, r20
        std     Z+1, r19                ; DDRx &= ~mask -- input
        ldd     r19, Z+2                ; PORTx
        or      r19, r18
        std     Z+2, r19                ; PORTx |= mask -- pull-up on
ir_begin_done:
        ret


; ---------------------------------------------------------------------------
; uint8_t ir_available(void)
;
; 1 once a complete frame has been decoded and not yet consumed by ir_resume.
; ---------------------------------------------------------------------------
ir_available:
        lds     r24, 0x0380
        ldi     r25, 0
        ret


; ---------------------------------------------------------------------------
; uint8_t ir_read_command(void) -- command byte of the last decoded frame.
; ---------------------------------------------------------------------------
ir_read_command:
        lds     r24, 0x0383
        ldi     r25, 0
        ret


; ---------------------------------------------------------------------------
; uint8_t ir_read_address(void) -- address byte of the last decoded frame.
; ---------------------------------------------------------------------------
ir_read_address:
        lds     r24, 0x0382
        ldi     r25, 0
        ret


; ---------------------------------------------------------------------------
; void ir_resume(void)
;
; Clears the available flag so the next frame can be reported. The stored
; address and command are left alone, so a repeat frame still has something to
; repeat.
; ---------------------------------------------------------------------------
ir_resume:
        ldi     r19, 0
        sts     0x0380, r19
        ret


; ---------------------------------------------------------------------------
; uint8_t ir_decode(void)
;
; Waits for a frame, measures it and stores it. Returns 1 on a good frame
; (including a repeat, which re-reports the previous code), 0 on timeout or a
; failed inverse check.
;
; The line is assumed idle-high on entry. Arriving mid-frame simply fails the
; leader check and costs one timeout.
;
; ir_fail sits ahead of the routine, and ir_bit_fail trampolines to it from
; inside the bit loop, because a conditional branch reaches only 63 words.
; ---------------------------------------------------------------------------
ir_fail:
        ldi     r24, 0
        ldi     r25, 0
        ret

ir_decode:
        lds     r19, 0x0384
        tst     r19
        breq    ir_fail                 ; ir_begin was never called
        rcall   ir_pin_setup
        tst     r18
        breq    ir_fail                 ; pin number out of range

        rcall   ir_wait_mark            ; r23 = 1 if a mark started
        tst     r23
        breq    ir_fail

        rcall   ir_measure_mark         ; the 9 ms leader mark
        cpi     r20, 135
        brlo    ir_fail
        cpi     r20, 226
        brsh    ir_fail

        rcall   ir_measure_space        ; 4.5 ms frame, or 2.25 ms repeat
        cpi     r20, 34
        brlo    ir_fail
        cpi     r20, 57
        brlo    ir_repeat
        cpi     r20, 67
        brlo    ir_fail
        cpi     r20, 113
        brsh    ir_fail

        ; ---- 32 data bits, LSB first ---------------------------------------
        ; Each measured bit is pushed in at the top of r25 and the whole
        ; 32-bit chain shifted right, so after 32 shifts the first bit
        ; received sits in bit 0 of r22 -- exactly LSB-first order, with
        ; r22 = address, r23 = ~address, r24 = command, r25 = ~command.
        ldi     r26, 32
        rjmp    ir_bit_loop
ir_bit_fail:
        rjmp    ir_fail
ir_bit_loop:
        rcall   ir_measure_mark         ; every bit opens with a 562.5 us mark
        cpi     r20, 7
        brlo    ir_bit_fail
        cpi     r20, 16
        brsh    ir_bit_fail
        rcall   ir_measure_space        ; the space carries the value
        cpi     r20, 7
        brlo    ir_bit_fail
        cpi     r20, 16
        brlo    ir_bit_zero             ; 7..15 ticks -> a 0
        cpi     r20, 24
        brlo    ir_bit_fail             ; between the two windows -- noise
        cpi     r20, 45
        brsh    ir_bit_fail
        sec                           ; 24..44 ticks -> a 1
        rjmp    ir_bit_shift
ir_bit_zero:
        clc
ir_bit_shift:
        ror     r25                     ; carry -> bit 7 of r25
        ror     r24
        ror     r23
        ror     r22                     ; bit 0 of r22 holds the first bit
        dec     r26
        brne    ir_bit_loop

        ; ---- checksum: command and its inverse must agree ------------------
        mov     r19, r24
        com     r19
        cp      r19, r25
        brne    ir_bit_fail

        sts     0x0382, r22             ; address
        sts     0x0383, r24             ; command
        ldi     r19, 1
        sts     0x0380, r19
        ldi     r24, 1
        ldi     r25, 0
        ret

; A repeat frame carries no data; the previously stored code stands.
ir_repeat:
        ldi     r19, 1
        sts     0x0380, r19
        ldi     r24, 1
        ldi     r25, 0
        ret


; ---------------------------------------------------------------------------
; ir_measure_mark: count 50 us ticks while the line stays LOW (a mark).
;
;   in:  Z = PINx, r18 = pin mask
;   out: r20 = ticks, 250 meaning the pulse never ended
;   clobbers r19, r20, r21
;
; The loop body is cycle-counted to 801 cycles, 50.06 us at 16 MHz:
;
;       ldd 2 + and 1 + branch not taken 1 + ldi 1
;       + inner (4 * 198 - 1 = 791) + inc 1 + cpi 1 + breq 1 + rjmp 2
;
; The inner loop is nop + dec + brne = 4 cycles per pass, so 198 passes is
; the bulk of the tick and the surrounding nine cycles round it out.
; ---------------------------------------------------------------------------
ir_measure_mark:
        ldi     r20, 0
ir_mm_loop:
        ldd     r19, Z+0                ; PINx
        and     r19, r18
        brne    ir_mm_done              ; line went high -- the mark ended
        ldi     r21, 198
ir_mm_delay:
        nop
        dec     r21
        brne    ir_mm_delay
        inc     r20
        cpi     r20, 250
        breq    ir_mm_done              ; stuck low -- give up
        rjmp    ir_mm_loop
ir_mm_done:
        ret


; ---------------------------------------------------------------------------
; ir_measure_space: count 50 us ticks while the line stays HIGH (a space).
;
; Identical timing to ir_measure_mark; only the sense of the test differs.
; ---------------------------------------------------------------------------
ir_measure_space:
        ldi     r20, 0
ir_ms_loop:
        ldd     r19, Z+0                ; PINx
        and     r19, r18
        breq    ir_ms_done              ; line went low -- the space ended
        ldi     r21, 198
ir_ms_delay:
        nop
        dec     r21
        brne    ir_ms_delay
        inc     r20
        cpi     r20, 250
        breq    ir_ms_done              ; nothing is coming
        rjmp    ir_ms_loop
ir_ms_done:
        ret


; ---------------------------------------------------------------------------
; ir_wait_mark: wait for the line to fall, i.e. for a frame to start.
;
;   in:  Z = PINx, r18 = pin mask
;   out: r23 = 1 if a mark started, 0 on timeout
;   clobbers r19, r21, r23, r26, r27
;
; Same 50 us tick as the measuring loops, counted 16-bit so the wait can run
; to 2000 ticks -- about 100 ms, long enough to catch a keypress without
; stalling a sketch's loop() for a noticeable time.
; ---------------------------------------------------------------------------
ir_wait_mark:
        ldi     r26, 0xD0               ; 2000 ticks
        ldi     r27, 0x07
ir_wm_loop:
        ldd     r19, Z+0                ; PINx
        and     r19, r18
        breq    ir_wm_found             ; line is low -- a mark has begun
        ldi     r21, 198
ir_wm_delay:
        nop
        dec     r21
        brne    ir_wm_delay
        sbiw    r26, 1
        brne    ir_wm_loop
        ldi     r23, 0
        ret
ir_wm_found:
        ldi     r23, 1
        ret


; ---------------------------------------------------------------------------
; ir_pin_setup: turn the stored pin number into a port pointer and a mask.
;
;   out: Z (r31:r30) = address of PINx, r18 = bit mask, or both 0 if the
;        stored number is not a pin on this part
;   clobbers r0, r18, r19, r24, r30, r31
;
; The numbering is not compiled in. __ardio_pinmap, which the device
; description renders alongside the runtime, holds two bytes per digital pin --
; the data-space address of its PINx register, then the bit number -- so pin n
; is at byte address __ardio_pinmap * 2 + n * 2, doubled because a label names
; a word and lpm addresses bytes. A pin the part does not have is stored as
; address 0, and every caller here already treats a zero mask as "give up",
; which is what the old out-of-range branch did.
;
; DDRx is at PINx+1 and PORTx at PINx+2 on every AVR, which is why ir_begin can
; reach both through the displacements Z+1 and Z+2 off this one pointer.
;
; The offset is built as a full 16-bit value rather than by shifting the pin
; number where it sits, so a board numbering past 127 pins cannot lose the top
; bit and read a different pin's entry.
ir_pin_setup:
        lds     r24, 0x0381
        ldi     r18, 0                  ; assume there is no such pin
        ldi     r30, 0
        ldi     r31, 0
        cpi     r24, AD_NUM_PINS
        brsh    ir_ps_done
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
        breq    ir_ps_done              ; address 0: not a pin on this part
        ldi     r18, 1                  ; mask = 1 << bit number
ir_ps_shift:
        tst     r0
        breq    ir_ps_done
        lsl     r18
        dec     r0
        rjmp    ir_ps_shift
ir_ps_done:
        ret
