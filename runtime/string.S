; ardio string runtime for the ATmega328P.
;
; The character primitives behind runtime/include/WString.h, written straight
; against the AVR instruction set and assembled by ardio's own assembler
; (src/avr/assembler.cpp) -- no external toolchain, no third-party sources.
;
; Calling convention matches avr-gcc:
;   * first int/pointer argument in r24:r25, second in r22:r23, third in r20:r21
;   * return value in r24:r25
;   * r1 is permanently zero
;   * r18-r27, r30, r31 are call-clobbered; r2-r17, r28, r29 are preserved
;
; Every routine here stays inside the call-clobbered set, so nothing has to be
; pushed on entry. Y (r28:r29) is call-saved and is deliberately never touched;
; the two pointers a routine needs live in X (r27:r26) and Z (r31:r30).
;
; CAPACITY, EVERYWHERE, MEANS THE SIZE OF THE WHOLE DESTINATION BUFFER --
; the terminator included. str_copy and str_append therefore write at most
; capacity-1 characters plus a NUL, and truncate rather than run past the end.
; A capacity of 0 leaves the buffer completely alone, because there is not even
; room for a terminator.


; ------------------------------------------------------------- str_len ---
; uint16_t str_len(const char *s)
;   s in r24:r25, length returned in r24:r25.
str_len:
        movw    r30, r24                ; Z = s
        ldi     r24, 0
        ldi     r25, 0
str_len_loop:
        ld      r18, Z+
        tst     r18
        breq    str_len_done
        adiw    r24, 1
        rjmp    str_len_loop
str_len_done:
        ret


; ------------------------------------------------------------ str_copy ---
; void str_copy(char *dst, const char *src, uint16_t capacity)
;   dst in r24:r25, src in r22:r23, capacity in r20:r21.
;
; Copies up to capacity-1 characters and always terminates. Nothing is written
; at all when capacity is 0.
str_copy:
        movw    r26, r24                ; X = dst
        movw    r30, r22                ; Z = src
        movw    r18, r20                ; r19:r18 = characters still allowed
        cp      r18, r1
        cpc     r19, r1
        breq    str_copy_out            ; no room even for the terminator
        subi    r18, 1                  ; reserve the terminator
        sbci    r19, 0
str_copy_loop:
        cp      r18, r1
        cpc     r19, r1
        breq    str_copy_end            ; budget spent: truncate here
        ld      r20, Z+
        tst     r20
        breq    str_copy_end
        st      X+, r20
        subi    r18, 1
        sbci    r19, 0
        rjmp    str_copy_loop
str_copy_end:
        st      X, r1                   ; r1 is the zero register
str_copy_out:
        ret


; ---------------------------------------------------------- str_append ---
; void str_append(char *dst, const char *src, uint16_t capacity)
;   dst in r24:r25, src in r22:r23, capacity in r20:r21.
;
; Walks to the end of dst spending the same budget, so a dst that already
; fills the buffer simply leaves nothing to append. Truncates, never overflows.
str_append:
        movw    r26, r24                ; X = dst
        movw    r30, r22                ; Z = src
        movw    r18, r20                ; r19:r18 = characters still allowed
        cp      r18, r1
        cpc     r19, r1
        breq    str_append_out
        subi    r18, 1                  ; reserve the terminator
        sbci    r19, 0
str_append_scan:
        cp      r18, r1
        cpc     r19, r1
        breq    str_append_end          ; dst already fills the buffer
        ld      r21, X
        tst     r21
        breq    str_append_copy         ; X now points at dst's terminator
        adiw    r26, 1
        subi    r18, 1
        sbci    r19, 0
        rjmp    str_append_scan
str_append_copy:
        cp      r18, r1
        cpc     r19, r1
        breq    str_append_end
        ld      r21, Z+
        tst     r21
        breq    str_append_end
        st      X+, r21
        subi    r18, 1
        sbci    r19, 0
        rjmp    str_append_copy
str_append_end:
        st      X, r1
str_append_out:
        ret


; --------------------------------------------------------- str_compare ---
; int16_t str_compare(const char *a, const char *b)
;   a in r24:r25, b in r22:r23. Returns the difference of the first pair of
;   characters that differ, treated as unsigned bytes, or 0 when equal --
;   the sign is what callers actually use.
str_compare:
        movw    r26, r24                ; X = a
        movw    r30, r22                ; Z = b
str_compare_loop:
        ld      r18, X+
        ld      r19, Z+
        cp      r18, r19
        brne    str_compare_diff
        tst     r18
        breq    str_compare_same        ; both ended together
        rjmp    str_compare_loop
str_compare_diff:
        mov     r24, r18
        ldi     r25, 0
        mov     r20, r19
        ldi     r21, 0
        sub     r24, r20
        sbc     r25, r21
        ret
str_compare_same:
        ldi     r24, 0
        ldi     r25, 0
        ret


; -------------------------------------------------------- str_from_int ---
; void str_from_int(char *dst, int16_t value)
;   dst in r24:r25, value in r22:r23.
;
; Digits come out least significant first, so they are pushed on the stack and
; popped back in order. Sixteen bits is five digits at most, plus a sign, so
; the deepest this goes is six bytes of stack.
;
; dst needs room for 7 bytes ("-32768" plus the terminator) in the worst case.
str_from_int:
        movw    r26, r24                ; X = dst
        tst     r23
        brpl    str_from_int_abs        ; already non-negative
        ldi     r20, 45                 ; '-'
        st      X+, r20
        com     r22                     ; value = -value, 16-bit
        com     r23
        subi    r22, 0xFF               ; ...+ 1
        sbci    r23, 0xFF
str_from_int_abs:
        ldi     r21, 0                  ; digits pushed so far
str_from_int_digits:
        rcall   __ardio_udiv10          ; r23:r22 /= 10, remainder in r18
        subi    r18, 0xD0               ; remainder + 48 -> ASCII digit
        push    r18
        inc     r21
        cp      r22, r1
        cpc     r23, r1
        brne    str_from_int_digits     ; -32768 negates to 32768 and still works
str_from_int_pop:
        pop     r18
        st      X+, r18
        dec     r21
        brne    str_from_int_pop
        st      X, r1
        ret


; Divides r23:r22 by 10. Quotient replaces r23:r22, remainder lands in r18.
;
; Restoring shift-and-subtract long division: sixteen passes, each shifting the
; next dividend bit into the remainder and subtracting the divisor when it
; fits. The remainder never exceeds 2*9+1 = 19, so a single byte holds it.
; Clobbers r18, r19, r22, r23 -- and nothing else, so callers can keep state
; in the other clobberable registers across the call.
__ardio_udiv10:
        ldi     r18, 0                  ; remainder
        ldi     r19, 16                 ; bits left
__ardio_udiv10_loop:
        lsl     r22
        rol     r23
        rol     r18                     ; the bit shifted out of the dividend
        cpi     r18, 10
        brlo    __ardio_udiv10_next
        subi    r18, 10
        ori     r22, 1                  ; quotient bit, into the vacated bit 0
__ardio_udiv10_next:
        dec     r19
        brne    __ardio_udiv10_loop
        ret


; ------------------------------------------------------- str_index_of ---
; int16_t str_index_of(const char *s, char c)
;   s in r24:r25, c in r22. Returns the index of the first c, or -1.
str_index_of:
        movw    r30, r24                ; Z = s
        ldi     r24, 0
        ldi     r25, 0
str_index_of_loop:
        ld      r18, Z+
        cp      r18, r22
        breq    str_index_of_done
        tst     r18
        breq    str_index_of_missing
        adiw    r24, 1
        rjmp    str_index_of_loop
str_index_of_missing:
        ldi     r24, 0xFF               ; -1
        ldi     r25, 0xFF
str_index_of_done:
        ret


; --------------------------------------------------------- str_char_at ---
; uint8_t str_char_at(const char *s, uint16_t index)
;   s in r24:r25, index in r22:r23, character returned in r24.
;
; Walks rather than adding the index to the pointer, so an index past the end
; returns 0 instead of reading whatever follows the string.
str_char_at:
        movw    r30, r24                ; Z = s
        ldi     r24, 0                  ; the out-of-range answer
str_char_at_loop:
        cp      r22, r1
        cpc     r23, r1
        breq    str_char_at_take
        ld      r18, Z+
        tst     r18
        breq    str_char_at_done        ; index is past the terminator
        subi    r22, 1
        sbci    r23, 0
        rjmp    str_char_at_loop
str_char_at_take:
        ld      r24, Z
str_char_at_done:
        ldi     r25, 0
        ret
