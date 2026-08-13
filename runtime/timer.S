; ardio timekeeping, at 16 MHz.
;
; Written from the megaAVR datasheets (Timer/Counter0 and the interrupt vector
; table) and assembled by ardio's own assembler -- no external toolchain, no
; third-party sources.
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
; The timer registers are named rather than numbered -- AD_TCCR0A, AD_TCCR0B,
; AD_TCNT0, AD_TIMSK0 and AD_TIFR0 come from the device description -- and are
; reached with lds/sts, which works whatever address a part gives them. TCNT0
; and TIFR0 used to be read with the one-cycle in and sbic forms, which is a
; word and a cycle cheaper but only legal below 0x20 and 0x40 of I/O space; the
; assembler cannot check that for a symbolic address, and an out-of-range I/O
; encoding silently aliases onto some other register. The two extra cycles
; inside micros()'s critical section are the price, and they are nowhere near
; long enough to lose a timer tick.
;
; SREG is the exception: its address is 0x3F on every AVR8 part, so the handler
; and the two critical sections below still use in/out for it. AD_SREG is an
; I/O address, unlike every other AD_* name, because in/out is the only way it
; is ever reached. It is written out at each use rather than given a shorter
; name with .equ, because a .equ would be evaluated as the assembler walks this
; file, which is before the device description at the end of the image is read.

.equ    TOV0,           0               ; overflow flag, bit 0 of TIFR0
.equ    CS01_CS00,      0x03            ; TCCR0B clock select: prescaler /64
.equ    TOIE0,          0x01            ; TIMSK0 bit 0: overflow interrupt

; State, placed by hand in SRAM. There is no linker and no .bss, so the nine
; bytes of state sit at the very bottom of SRAM, wherever the part puts it:
;
;       AD_RAMSTART+0   4 bytes, milliseconds since reset
;       AD_RAMSTART+4   4 bytes, raw Timer0 overflow count
;       AD_RAMSTART+8   1 byte, the 1/125 ms remainder
;
; The stack grows down from AD_RAMEND, so the two ends never meet. The
; addresses are written out as AD_RAMSTART+n at each use rather than given
; names with .equ, because .equ is resolved as the assembler walks the file and
; the device description is emitted after the runtime, so a name defined this
; way would be evaluated before AD_RAMSTART exists.


; ------------------------------------------------- interrupt vectors ---
; The ATmega328P vector table is 26 vectors of two words each, occupying word
; addresses 0x0000 to 0x0033. Vector 0 is RESET and vector 16 is TIMER0_OVF,
; at word address 16 * 2 = 0x0020.
;
; These three .org addresses are the last part-specific numbers in the runtime,
; and they cannot become AD_* names as things stand. .org has to be resolved
; while the assembler is still measuring where everything lands, which happens
; before the device description at the end of the image has been read, so a
; symbolic vector number would be an undefined name rather than a table entry.
; Every other part in the device table puts TIMER0_OVF somewhere else -- vector
; 22 on the ATmega32U4, 23 on the ATmega2560, 9 on the ATmega8 -- so this file
; is still a 328P file, and is excluded from the runtime the build assembles
; for that reason.
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
        sts     AD_TCCR0A, r24          ; normal mode: WGM01:0 = 00, no output
        sts     AD_TCNT0, r24           ; start counting from zero
        sts     AD_RAMSTART+0, r24
        sts     AD_RAMSTART+1, r24
        sts     AD_RAMSTART+2, r24
        sts     AD_RAMSTART+3, r24
        sts     AD_RAMSTART+4, r24
        sts     AD_RAMSTART+5, r24
        sts     AD_RAMSTART+6, r24
        sts     AD_RAMSTART+7, r24
        sts     AD_RAMSTART+8, r24
        ldi     r24, CS01_CS00
        sts     AD_TCCR0B, r24          ; clock the timer at 16 MHz / 64
        ldi     r24, TOIE0
        sts     AD_TIMSK0, r24          ; interrupt on every wrap
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
; carried in AD_RAMSTART+8 in units of 1/125 ms (8 us): 24 us is exactly 3 of
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
        in      r24, AD_SREG
        push    r24                     ; the interrupted code's flags
        push    r25
        push    r26
        clr     r26                     ; a zero to carry into the high bytes

        lds     r24, AD_RAMSTART+8
        subi    r24, -3                 ; fract += 3   (3/125 ms = 24 us)
        ldi     r25, 1                  ; milliseconds to add this time
        cpi     r24, 125
        brlo    __ardio_ovf_fract
        subi    r24, 125                ; a whole millisecond accumulated
        ldi     r25, 2
__ardio_ovf_fract:
        sts     AD_RAMSTART+8, r24

        ; ARDIO_MILLIS += r25, carried up through all four bytes. Neither lds
        ; nor sts touches the flags, so the carry from each add survives to
        ; the next adc.
        lds     r24, AD_RAMSTART+0
        add     r24, r25
        sts     AD_RAMSTART+0, r24
        lds     r24, AD_RAMSTART+1
        adc     r24, r26
        sts     AD_RAMSTART+1, r24
        lds     r24, AD_RAMSTART+2
        adc     r24, r26
        sts     AD_RAMSTART+2, r24
        lds     r24, AD_RAMSTART+3
        adc     r24, r26
        sts     AD_RAMSTART+3, r24

        ; ARDIO_OVF += 1, as a subtraction of -1 so the borrow chains with
        ; sbci exactly the way the carry chains with adc above.
        lds     r24, AD_RAMSTART+4
        subi    r24, 0xFF
        sts     AD_RAMSTART+4, r24
        lds     r24, AD_RAMSTART+5
        sbci    r24, 0xFF
        sts     AD_RAMSTART+5, r24
        lds     r24, AD_RAMSTART+6
        sbci    r24, 0xFF
        sts     AD_RAMSTART+6, r24
        lds     r24, AD_RAMSTART+7
        sbci    r24, 0xFF
        sts     AD_RAMSTART+7, r24

        pop     r26
        pop     r25
        pop     r24
        out     AD_SREG, r24            ; flags back exactly as they were
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
        in      r18, AD_SREG            ; remember the I bit
        cli
        lds     r22, AD_RAMSTART+0
        lds     r23, AD_RAMSTART+1
        lds     r24, AD_RAMSTART+2
        lds     r25, AD_RAMSTART+3
        out     AD_SREG, r18            ; interrupts back on if they were on
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
        in      r18, AD_SREG
        cli
        lds     r19, AD_RAMSTART+4
        lds     r20, AD_RAMSTART+5
        lds     r21, AD_RAMSTART+6
        lds     r26, AD_TCNT0           ; same instant as the loads above
        lds     r27, AD_TIFR0
        sbrc    r27, TOV0               ; a wrap the handler has not seen yet
        rjmp    __ardio_micros_pending
__ardio_micros_have:
        out     AD_SREG, r18            ; end of the critical section
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
        out     AD_SREG, r1             ; interrupts off, flags clear
        ldi     r28, lo8(AD_RAMEND)     ; the stack grows down from the top
        out     AD_SPL, r28
        ldi     r28, hi8(AD_RAMEND)
        out     AD_SPH, r28
        rcall   timer_init
        call    setup
__ardio_fallback_loop:
        call    loop
        rjmp    __ardio_fallback_loop
setup:
        ret
loop:
        ret
