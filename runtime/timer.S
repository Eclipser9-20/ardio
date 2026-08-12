; ardio timekeeping for the ATmega328P at 16 MHz.
;
; Written from the ATmega328P datasheet (Timer/Counter0 and the interrupt
; vector table) and assembled by ardio's own assembler -- no external
; toolchain, no third-party sources.
;
; Timer/Counter0 is an 8-bit free-running counter. With a /64 prescaler it is
; clocked at 16 MHz / 64 = 250 kHz, so one timer tick is 4 us and a full
; 256-tick wrap is 1024 us -- 1.024 ms, not 1.000 ms. That 24 us of slack per
; overflow is what the fractional accumulator below exists to give back.
;
; Calling convention matches avr-gcc:
;   * first int argument in r24:r25
;   * a 32-bit result comes back in r22..r25, low byte in r22
;   * r1 is permanently zero
;   * r18-r27, r30, r31 are call-clobbered; r2-r17, r28, r29 are preserved
;
; Register addresses (datasheet register summary). Anything at 0x60 or above
; is outside the in/out I/O window and has to be reached with lds/sts.
;
;       TCCR0A 0x44   TCCR0B 0x45   TCNT0 0x46   TIFR0 0x35   TIMSK0 0x6E
;
; The I/O-space alias of a register is its data address minus 0x20, so TCNT0
; is I/O 0x26 and TIFR0 is I/O 0x15 -- both reachable with the one-cycle in /
; sbic forms, which matters inside the interrupt handler and in micros().

.equ    TCCR0A,         0x44
.equ    TCCR0B,         0x45
.equ    TCNT0,          0x46
.equ    TIMSK0,         0x6E
.equ    TCNT0_IO,       0x26            ; TCNT0 - 0x20, for in
.equ    TIFR0_IO,       0x15            ; TIFR0 - 0x20, for sbic
.equ    TOV0,           0               ; overflow flag, bit 0 of TIFR0
.equ    SREG_IO,        0x3F

.equ    CS01_CS00,      0x03            ; TCCR0B clock select: prescaler /64
.equ    TOIE0,          0x01            ; TIMSK0 bit 0: overflow interrupt

; State, placed by hand in SRAM. There is no linker and no .bss, so the few
; bytes of state get fixed addresses at the bottom of SRAM (0x0100 is the
; first internal SRAM byte on this part; the stack grows down from 0x08FF, so
; the two ends never meet).
.equ    ARDIO_MILLIS,   0x0100          ; 4 bytes, milliseconds since reset
.equ    ARDIO_OVF,      0x0104          ; 4 bytes, raw Timer0 overflow count
.equ    ARDIO_FRACT,    0x0108          ; 1 byte, the 1/125 ms remainder


; ------------------------------------------------- interrupt vectors ---
; The ATmega328P vector table is 26 vectors of two words each, occupying word
; addresses 0x0000 to 0x0033. Vector 0 is RESET and vector 16 is TIMER0_OVF,
; at word address 16 * 2 = 0x0020.
;
; Only one file can own word address 0. When timer.S is assembled it takes
; that job: the reset vector jumps straight to __ardio_start, the entry point
; runtime/core.S defines, so core.S keeps being the thing that actually runs
; at reset -- it just no longer has to sit at address zero itself. Every
; unused vector is left as erased flash (0xFF), which decodes to `sbrs r31, 7`
; and simply falls through the table into the code below; no unused interrupt
; is ever enabled, so nothing reaches those words.
.org    0x0000
        jmp     __ardio_start           ; vector 0: RESET

.org    0x0020
        jmp     __ardio_timer0_ovf      ; vector 16: TIMER0_OVF

.org    0x0034                          ; first word past the table


; --------------------------------------------------------- timer_init ---
; void timer_init(void)
;
; Normal mode (no waveform generation), prescaler /64, overflow interrupt on,
; counters cleared. Global interrupts are enabled on the way out, so a sketch
; only has to call this once from setup().
timer_init:
        ldi     r24, 0
        sts     TCCR0A, r24             ; normal mode: WGM01:0 = 00, no output
        sts     TCNT0, r24              ; start counting from zero
        sts     ARDIO_MILLIS+0, r24
        sts     ARDIO_MILLIS+1, r24
        sts     ARDIO_MILLIS+2, r24
        sts     ARDIO_MILLIS+3, r24
        sts     ARDIO_OVF+0, r24
        sts     ARDIO_OVF+1, r24
        sts     ARDIO_OVF+2, r24
        sts     ARDIO_OVF+3, r24
        sts     ARDIO_FRACT, r24
        ldi     r24, CS01_CS00
        sts     TCCR0B, r24             ; clock the timer at 16 MHz / 64
        ldi     r24, TOIE0
        sts     TIMSK0, r24             ; interrupt on every wrap
        sei
        ret


; ------------------------------------------------ overflow interrupt ---
; Runs every 1024 us. Two counters are kept:
;
;   ARDIO_OVF    the raw number of wraps, which is what micros() scales
;   ARDIO_MILLIS whole milliseconds, which is what millis() returns
;
; 1024 us is 1 ms plus 24 us, so the millisecond counter cannot simply be
; incremented once per overflow -- that would run 2.4% slow. The remainder is
; carried in ARDIO_FRACT in units of 1/125 ms (8 us): 24 us is exactly 3 of
; them, and 125 of them make a millisecond. So each overflow adds 3, and when
; the accumulator reaches 125 it gives back one extra millisecond and keeps
; the difference. No division, no drift.
;
; An interrupt handler must leave the machine exactly as it found it, so every
; register touched is pushed, and SREG with them -- the handler runs between
; arbitrary instructions, and clobbering the flags would corrupt an addition
; or a branch in whatever code was interrupted.
__ardio_timer0_ovf:
        push    r24
        in      r24, SREG_IO
        push    r24                     ; the interrupted code's flags
        push    r25
        push    r26
        clr     r26                     ; a zero to carry into the high bytes

        lds     r24, ARDIO_FRACT
        subi    r24, -3                 ; fract += 3   (3/125 ms = 24 us)
        ldi     r25, 1                  ; milliseconds to add this time
        cpi     r24, 125
        brlo    __ardio_ovf_fract
        subi    r24, 125                ; a whole millisecond accumulated
        ldi     r25, 2
__ardio_ovf_fract:
        sts     ARDIO_FRACT, r24

        ; ARDIO_MILLIS += r25, carried up through all four bytes. Neither lds
        ; nor sts touches the flags, so the carry from each add survives to
        ; the next adc.
        lds     r24, ARDIO_MILLIS+0
        add     r24, r25
        sts     ARDIO_MILLIS+0, r24
        lds     r24, ARDIO_MILLIS+1
        adc     r24, r26
        sts     ARDIO_MILLIS+1, r24
        lds     r24, ARDIO_MILLIS+2
        adc     r24, r26
        sts     ARDIO_MILLIS+2, r24
        lds     r24, ARDIO_MILLIS+3
        adc     r24, r26
        sts     ARDIO_MILLIS+3, r24

        ; ARDIO_OVF += 1, as a subtraction of -1 so the borrow chains with
        ; sbci exactly the way the carry chains with adc above.
        lds     r24, ARDIO_OVF+0
        subi    r24, 0xFF
        sts     ARDIO_OVF+0, r24
        lds     r24, ARDIO_OVF+1
        sbci    r24, 0xFF
        sts     ARDIO_OVF+1, r24
        lds     r24, ARDIO_OVF+2
        sbci    r24, 0xFF
        sts     ARDIO_OVF+2, r24
        lds     r24, ARDIO_OVF+3
        sbci    r24, 0xFF
        sts     ARDIO_OVF+3, r24

        pop     r26
        pop     r25
        pop     r24
        out     SREG_IO, r24            ; flags back exactly as they were
        pop     r24
        reti


; -------------------------------------------------------------- millis ---
; uint32_t millis(void)
;   returns in r22..r25, low byte in r22
;
; The counter is four bytes wide but the processor reads one byte at a time.
; If the overflow interrupt fired between two of those loads the result would
; be torn: the low bytes from before the tick and the high bytes from after
; it, which around a carry boundary (0x00FF -> 0x0100) yields a value that was
; never on the clock, and can read *backwards*. Code that does
; `if (millis() - start >= interval)` then hangs for 49 days.
;
; So the four loads are made indivisible by clearing the global interrupt
; enable for their duration. Rather than blindly running sei afterwards, SREG
; is saved first and written back after -- restoring the I bit to whatever it
; was. That is what makes millis() safe to call from inside an interrupt
; handler too: it will not silently re-enable interrupts on a caller that had
; them off. The critical section is nine instructions, so an interrupt is
; delayed by well under a microsecond and no timer tick can be lost.
millis:
        in      r18, SREG_IO            ; remember the I bit
        cli
        lds     r22, ARDIO_MILLIS+0
        lds     r23, ARDIO_MILLIS+1
        lds     r24, ARDIO_MILLIS+2
        lds     r25, ARDIO_MILLIS+3
        out     SREG_IO, r18            ; interrupts back on if they were on
        ret


; -------------------------------------------------------------- micros ---
; uint32_t micros(void)
;   returns in r22..r25, low byte in r22
;
; Elapsed timer ticks are (overflow count * 256) + TCNT0, and each tick is
; 4 us, so
;
;       micros = ((ovf * 256) + TCNT0) * 4
;
; The * 256 is free: it is just a byte shuffle, putting TCNT0 in the low byte
; and the overflow bytes above it (the top overflow byte falls off the end,
; which is the wrap at 2^32 us -- about 71.6 minutes -- that micros() has on
; every AVR core). The * 4 is two 32-bit left shifts.
;
; The same tearing problem applies here, plus one more: the counter and TCNT0
; must come from the same instant, and TCNT0 can wrap between them. Both reads
; therefore happen with interrupts off. If TCNT0 has already wrapped but the
; handler has not run yet, TOV0 is still pending in TIFR0, so the overflow
; that the handler has not counted yet is added by hand. TCNT0 = 0xFF is the
; one case where the flag belongs to the wrap that is about to happen rather
; than one already taken, and is left alone.
micros:
        in      r18, SREG_IO
        cli
        lds     r19, ARDIO_OVF+0
        lds     r20, ARDIO_OVF+1
        lds     r21, ARDIO_OVF+2
        in      r26, TCNT0_IO           ; same instant as the loads above
        sbic    TIFR0_IO, TOV0          ; skip ahead when nothing is pending
        rjmp    __ardio_micros_pending
__ardio_micros_have:
        out     SREG_IO, r18            ; end of the critical section
        mov     r22, r26                ; * 256: TCNT0 becomes the low byte
        mov     r23, r19
        mov     r24, r20
        mov     r25, r21
        lsl     r22                     ; * 2
        rol     r23
        rol     r24
        rol     r25
        lsl     r22                     ; * 2 again -> * 4, one tick = 4 us
        rol     r23
        rol     r24
        rol     r25
        ret
__ardio_micros_pending:
        cpi     r26, 0xFF               ; wrap not taken yet: leave it
        breq    __ardio_micros_have
        subi    r19, 0xFF               ; count the overflow the handler owes
        sbci    r20, 0xFF
        sbci    r21, 0xFF
        rjmp    __ardio_micros_have


; ARDIO_FALLBACK_BEGIN
; -------------------------------------------------------------- sketch ---
; Fallback definitions so this file assembles and links on its own. Everything
; below the ARDIO_FALLBACK_BEGIN marker is dropped when this file is combined
; with runtime/core.S, which defines the real __ardio_start, and with a sketch
; that supplies its own setup and loop.
__ardio_start:
        clr     r1                      ; the zero register, by convention
        out     0x3F, r1                ; SREG = 0: interrupts off, flags clear
        ldi     r28, 0xFF               ; RAMEND = 0x08FF on the ATmega328P
        out     0x3D, r28               ; SPL
        ldi     r28, 0x08
        out     0x3E, r28               ; SPH
        rcall   timer_init
        call    setup
__ardio_fallback_loop:
        call    loop
        rjmp    __ardio_fallback_loop
setup:
        ret
loop:
        ret
