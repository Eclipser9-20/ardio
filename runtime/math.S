; ardio 16-bit division and multiplication helpers.
;
; The AVR has MUL but no divide instruction at all, so `/` and `%` are compiled
; into calls to the routines below. They are written for ardio's own assembler
; (src/avr/assembler.cpp) and stick to the avr-gcc calling convention:
;
;   * arguments and results in r24:r25 and r22:r23
;   * r1 is permanently zero and is never disturbed here
;   * only call-clobbered registers (r18-r27, r30, r31) are touched, so no
;     routine needs a prologue
;
; Division is a plain restoring shift-subtract: sixteen times, shift the next
; bit of the dividend into the remainder, subtract the divisor when it fits,
; and record the quotient bit in the space the dividend just vacated. That is
; why the quotient comes back in the same register pair the dividend arrived
; in -- the two never overlap.


; ------------------------------------------------------ unsigned divmod ---
; Unsigned 16-bit division.
;
;   in:  r24:r25 = dividend, r22:r23 = divisor
;   out: r24:r25 = quotient, r18:r19 = remainder
;   clobbers r18, r19, r20
;
; A divisor of zero is not diagnosed -- it yields 0xFFFF, matching what the
; usual avr-libc routine does rather than trapping on a machine with no traps.
__ardio_udivmod16:
        clr     r18                     ; remainder = 0
        clr     r19
        ldi     r20, 16                 ; sixteen bits to go
__ardio_udivmod16_loop:
        lsl     r24                     ; dividend <<= 1, top bit into carry
        rol     r25
        rol     r18                     ; remainder = remainder<<1 | that bit
        rol     r19
        cp      r18, r22
        cpc     r19, r23
        brlo    __ardio_udivmod16_next  ; remainder < divisor: quotient bit 0
        sub     r18, r22                ; it fits: subtract and set the bit
        sbc     r19, r23
        ori     r24, 1                  ; the bit r24 just shifted out of
__ardio_udivmod16_next:
        dec     r20
        brne    __ardio_udivmod16_loop
        ret


; -------------------------------------------------------- signed divmod ---
; Signed 16-bit division, truncating toward zero, with the remainder taking
; the sign of the dividend -- the rule C requires.
;
;   in:  r24:r25 = dividend, r22:r23 = divisor
;   out: r24:r25 = quotient, r18:r19 = remainder
;   clobbers r18, r19, r20, r21, r26
;
; The signs are recorded in r21 before the magnitudes go through the unsigned
; routine: bit 0 means "negate the quotient", bit 1 means "negate the
; remainder". A negative dividend sets both; a negative divisor flips only the
; quotient's bit, so two negatives cancel.
__ardio_divmod16:
        ldi     r21, 0
        sbrs    r25, 7
        rjmp    __ardio_divmod16_num_ok
        com     r25                     ; dividend = -dividend
        neg     r24
        sbci    r25, -1
        ldi     r21, 3                  ; negate both results
__ardio_divmod16_num_ok:
        sbrs    r23, 7
        rjmp    __ardio_divmod16_den_ok
        com     r23                     ; divisor = -divisor
        neg     r22
        sbci    r23, -1
        ldi     r26, 1
        eor     r21, r26                ; flip only the quotient's sign
__ardio_divmod16_den_ok:
        call    __ardio_udivmod16
        sbrs    r21, 0
        rjmp    __ardio_divmod16_quot_ok
        com     r25                     ; quotient = -quotient
        neg     r24
        sbci    r25, -1
__ardio_divmod16_quot_ok:
        sbrs    r21, 1
        rjmp    __ardio_divmod16_done
        com     r19                     ; remainder = -remainder
        neg     r18
        sbci    r19, -1
__ardio_divmod16_done:
        ret


; ----------------------------------------------------------- multiplies ---
; Unsigned 16x16 -> 16 multiply. The code generator inlines this sequence for
; `*`, but index scaling by an awkward element size calls it instead of
; growing every subscript by six instructions.
;
;   in:  r24:r25 = a, r22:r23 = b
;   out: r24:r25 = a * b
;   clobbers r18, r19, r0, r1 (r1 is restored to zero)
__ardio_mul16:
        mul     r24, r22                ; low * low
        movw    r18, r0
        mul     r25, r22                ; high(a) * low(b)
        add     r19, r0
        mul     r24, r23                ; low(a) * high(b)
        add     r19, r0
        clr     r1                      ; MUL dirtied the zero register
        movw    r24, r18
        ret
