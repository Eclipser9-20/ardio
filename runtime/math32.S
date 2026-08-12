; ardio 32-bit multiply and division helpers.
;
; The code generator computes a `long` in r22..r25 -- the ABI's home for a
; 32-bit argument and for a 32-bit return value -- and puts the right-hand
; operand of a binary operation in r18..r21. These routines take their
; arguments exactly there, so `*`, `/` and `%` compile to a single CALL with no
; shuffling on either side.
;
;   left  operand / dividend : r22 r23 r24 r25   (low byte first)
;   right operand / divisor  : r18 r19 r20 r21
;
; Only call-clobbered registers (r18-r27, r30, r31) and the scratch registers
; r0 and r1 are touched, so none of these needs a prologue. r1 is the ABI's
; permanent zero: MUL destroys it, so every multiply here clears it again
; before returning.
;
; Written for ardio's own assembler (src/avr/assembler.cpp), which is why the
; only mnemonics used are the plain AVR ones -- no macros, no .macro, no
; assembler arithmetic beyond simple constants.


; ------------------------------------------------------------- multiply ---
; 32 x 32 -> low 32 multiply.
;
;   in:  r22..r25 = a, r18..r21 = b
;   out: r22..r25 = a * b   (modulo 2^32, which is what C requires)
;   clobbers r26, r27, r30, r31, r0, r1 (r1 is restored to zero)
;
; The product is accumulated in r26:r27:r30:r31 -- the four call-clobbered
; registers the operands do not occupy -- as the sum of the partial products
; ai*bj, skipping every pair with i+j > 3 because those land entirely above the
; 32 bits kept. MUL leaves its result in r1:r0, so each partial product is
; added, its high half is added with carry, r1 is cleared back to zero, and the
; remaining carry is then propagated with that freshly zeroed r1. CLR does not
; touch the carry flag, which is what makes that order safe.
__ardio_mul32:
        mul     r22, r18                ; a0*b0 -> bytes 0 and 1
        movw    r26, r0
        clr     r30
        clr     r31
        clr     r1

        mul     r23, r18                ; a1*b0 -> bytes 1 and 2
        add     r27, r0
        adc     r30, r1
        clr     r1
        adc     r31, r1

        mul     r22, r19                ; a0*b1 -> bytes 1 and 2
        add     r27, r0
        adc     r30, r1
        clr     r1
        adc     r31, r1

        mul     r24, r18                ; a2*b0 -> bytes 2 and 3
        add     r30, r0
        adc     r31, r1
        clr     r1

        mul     r23, r19                ; a1*b1 -> bytes 2 and 3
        add     r30, r0
        adc     r31, r1
        clr     r1

        mul     r22, r20                ; a0*b2 -> bytes 2 and 3
        add     r30, r0
        adc     r31, r1
        clr     r1

        mul     r25, r18                ; the four products that reach only
        add     r31, r0                 ; byte 3; their high halves fall off
        mul     r24, r19
        add     r31, r0
        mul     r23, r20
        add     r31, r0
        mul     r22, r21
        add     r31, r0
        clr     r1                      ; MUL dirtied the zero register

        movw    r22, r26
        movw    r24, r30
        ret


; ------------------------------------------------------ unsigned divmod ---
; Unsigned 32-bit division, by the same restoring shift-subtract as the 16-bit
; routine in math.S: thirty-two times, shift the next bit of the dividend into
; the remainder, subtract the divisor when it fits, and record the quotient bit
; in the low bit the dividend has just vacated. The quotient therefore grows
; into the register quartet the dividend arrived in.
;
;   in:  r22..r25 = dividend, r18..r21 = divisor
;   out: r22..r25 = quotient, r26:r27:r30:r31 = remainder (low byte first)
;   clobbers r26, r27, r30, r31, r0
;
; The remainder uses the four registers left over once the dividend and divisor
; have taken r18-r25; they are not contiguous, but no instruction here needs
; them to be. r0 holds the bit counter -- it is scratch by the ABI, and LDI
; cannot reach it, hence the load-then-move.
;
; A zero divisor is not diagnosed: it yields 0xFFFFFFFF, matching what the
; 16-bit routine does rather than trapping on a machine with no traps.
__ardio_udivmod32:
        clr     r26                     ; remainder = 0
        clr     r27
        clr     r30
        clr     r31
        ldi     r26, 32
        mov     r0, r26                 ; thirty-two bits to go
        clr     r26
__ardio_udivmod32_loop:
        lsl     r22                     ; dividend <<= 1, top bit into carry
        rol     r23
        rol     r24
        rol     r25
        rol     r26                     ; remainder = remainder<<1 | that bit
        rol     r27
        rol     r30
        rol     r31
        cp      r26, r18
        cpc     r27, r19
        cpc     r30, r20
        cpc     r31, r21
        brlo    __ardio_udivmod32_next  ; remainder < divisor: quotient bit 0
        sub     r26, r18                ; it fits: subtract and set the bit
        sbc     r27, r19
        sbc     r30, r20
        sbc     r31, r21
        ori     r22, 1                  ; the bit r22 just shifted out of
__ardio_udivmod32_next:
        dec     r0
        brne    __ardio_udivmod32_loop
        ret


; -------------------------------------------------------- signed divmod ---
; Signed 32-bit division, truncating toward zero, with the remainder taking the
; sign of the dividend -- the rule C requires.
;
;   in:  r22..r25 = dividend, r18..r21 = divisor
;   out: r22..r25 = quotient, r26:r27:r30:r31 = remainder
;   clobbers r18..r21, r26, r27, r30, r31, r0
;
; The signs are recorded before the magnitudes go through the unsigned routine:
; bit 0 means "negate the quotient", bit 1 means "negate the remainder". A
; negative dividend sets both; a negative divisor flips only the quotient's
; bit, so two negatives cancel. The byte is parked on the stack because the
; unsigned routine needs every register that is left.
;
; Each negation complements the top three bytes first and only then NEGs the
; low one: COM sets the carry unconditionally, so it must not run between the
; NEG and the SBCI chain that consumes that NEG's borrow.
__ardio_divmod32:
        ldi     r26, 0
        sbrs    r25, 7
        rjmp    __ardio_divmod32_num_ok
        com     r25                     ; dividend = -dividend
        com     r24
        com     r23
        neg     r22
        sbci    r23, -1
        sbci    r24, -1
        sbci    r25, -1
        ldi     r26, 3                  ; negate both results
__ardio_divmod32_num_ok:
        sbrs    r21, 7
        rjmp    __ardio_divmod32_den_ok
        com     r21                     ; divisor = -divisor
        com     r20
        com     r19
        neg     r18
        sbci    r19, -1
        sbci    r20, -1
        sbci    r21, -1
        ldi     r27, 1
        eor     r26, r27                ; flip only the quotient's sign
__ardio_divmod32_den_ok:
        push    r26
        call    __ardio_udivmod32
        pop     r18                     ; the divisor's registers are free now
        sbrs    r18, 0
        rjmp    __ardio_divmod32_quot_ok
        com     r25                     ; quotient = -quotient
        com     r24
        com     r23
        neg     r22
        sbci    r23, -1
        sbci    r24, -1
        sbci    r25, -1
__ardio_divmod32_quot_ok:
        sbrs    r18, 1
        rjmp    __ardio_divmod32_done
        com     r31                     ; remainder = -remainder
        com     r30
        com     r27
        neg     r26
        sbci    r27, -1
        sbci    r30, -1
        sbci    r31, -1
__ardio_divmod32_done:
        ret
