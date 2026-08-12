; ardio USART0 runtime for the ATmega328P at 16 MHz.
;
; ardio's own serial support, written straight from the ATmega328P datasheet
; USART chapter and assembled by ardio's own assembler (src/avr/assembler.cpp)
; -- no external toolchain, no third-party sources.
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
; USART0 lives above the 0x00-0x3F I/O space, so in/out cannot reach it and
; every access here is lds/sts against the data-space address (datasheet
; register summary):
;
;       UCSR0A 0xC0   UCSR0B 0xC1   UCSR0C 0xC2
;       UBRR0L 0xC4   UBRR0H 0xC5   UDR0   0xC6
;
; Bits used, by position:
;
;       UCSR0A: RXC0 7, TXC0 6, UDRE0 5, U2X0 1
;       UCSR0B: RXEN0 4, TXEN0 3          -> (1<<RXEN0)|(1<<TXEN0) = 0x18
;       UCSR0C: UCSZ01 2, UCSZ00 1        -> 8 data bits, no parity, 1 stop
;                                            = 0x06, with UMSEL0 = 0 (async)
;
; This is polled, not interrupt-driven: there is no ring buffer and no ISR, so
; a sketch never has to reason about a partly-drained queue. Sending blocks
; only while the single hardware transmit register is busy, which at 9600 baud
; is about a millisecond per character.


; ------------------------------------------------------------ begin ---
; void serial_begin(uint16_t ubrr)
;
; Takes the *baud divisor*, not the baud rate. A 32-bit baud argument would
; arrive in r22..r25 and would then need a 32-bit division by hand to become
; UBRR0; the divisor is a compile-time constant in every real sketch, so
; HardwareSerial::begin() does that conversion in the header and this routine
; takes the finished 16-bit value in r24:r25.
;
;   UBRR0 = F_CPU / (16 * baud) - 1   in normal (U2X0 = 0) mode
;   16 MHz, 9600 baud  -> 103
;   16 MHz, 115200 baud -> 8   (the usual 2.1% error; still within tolerance)
;
; The transmitter and receiver are switched off while the divisor changes, so
; a begin() in the middle of a session cannot leave a half-shifted character
; on the wire.
serial_begin:
        ldi     r18, 0
        sts     0xC1, r18               ; UCSR0B = 0: RX and TX off
        sts     0xC5, r25               ; UBRR0H  -- high byte first
        sts     0xC4, r24               ; UBRR0L  -- writing low latches both
        ldi     r18, 0x40
        sts     0xC0, r18               ; UCSR0A: clear TXC0, U2X0 = 0
        ldi     r18, 0x06
        sts     0xC2, r18               ; UCSR0C: 8N1, asynchronous
        ldi     r18, 0x18
        sts     0xC1, r18               ; UCSR0B: RXEN0 | TXEN0
        ret


; ------------------------------------------------------------ write ---
; void serial_write(uint8_t c)
;   c in r24. Blocks until the data register is empty, then hands the byte to
;   the transmitter. Clobbers only r18, which the printing helpers below rely
;   on -- they keep their state in r19-r27 across these calls.
serial_write:
        lds     r18, 0xC0               ; UCSR0A
        sbrs    r18, 5                  ; UDRE0 set means UDR0 is free
        rjmp    serial_write
        sts     0xC6, r24               ; UDR0
        ret


; ------------------------------------------------------------ print ---
; void serial_print(const char* s)
;   s in r24:r25, NUL-terminated, walked through Z with post-increment.
;   Z survives serial_write, so the pointer never needs saving.
serial_print:
        movw    r30, r24
serial_print_loop:
        ld      r24, Z+
        tst     r24
        breq    serial_print_done
        rcall   serial_write
        rjmp    serial_print_loop
serial_print_done:
        ret


; -------------------------------------------------------- print_int ---
; void serial_print_int(int16_t v)
;   v in r24:r25. Decimal, with a leading minus for negatives.
;
; There is no divide instruction on the AVR, so each decimal place is found by
; repeated subtraction of a power of ten -- five stages, at most nine
; subtractions each. The running value is held in r26:r27 as *unsigned*, which
; is what makes -32768 come out right: negating it overflows back to 0x8000,
; and 0x8000 read as unsigned is exactly the 32768 that should follow the sign.
;
;   r26:r27 = remaining value
;   r20:r21 = current power of ten
;   r22     = 1 once a digit has been emitted (leading-zero suppression)
;   r23     = digit being counted out
serial_print_int:
        movw    r26, r24
        ldi     r22, 0                  ; nothing printed yet
        sbrs    r27, 7                  ; sign bit of the high byte
        rjmp    serial_print_int_digits
        com     r26                     ; two's complement negate
        com     r27
        subi    r26, -1
        sbci    r27, -1
        ldi     r24, 45                 ; '-'
        rcall   serial_write
serial_print_int_digits:
        ldi     r20, 0x10               ; 10000
        ldi     r21, 0x27
        rcall   __serial_digit
        ldi     r20, 0xE8               ; 1000
        ldi     r21, 0x03
        rcall   __serial_digit
        ldi     r20, 100
        ldi     r21, 0
        rcall   __serial_digit
        ldi     r20, 10
        ldi     r21, 0
        rcall   __serial_digit
        mov     r24, r26                ; the units digit is what is left
        subi    r24, -48                ; + '0'
        rcall   serial_write
        ret

; One decimal place: count how many times r20:r21 fits into r26:r27, print
; that digit unless it is a leading zero, and leave the remainder behind.
__serial_digit:
        ldi     r23, 0
__serial_digit_loop:
        cp      r26, r20
        cpc     r27, r21
        brlo    __serial_digit_emit     ; value < power of ten: done counting
        sub     r26, r20
        sbc     r27, r21
        inc     r23
        rjmp    __serial_digit_loop
__serial_digit_emit:
        tst     r23
        brne    __serial_digit_print
        tst     r22
        breq    __serial_digit_done     ; leading zero: suppress it
__serial_digit_print:
        ldi     r22, 1
        mov     r24, r23
        subi    r24, -48                ; + '0'
        rcall   serial_write
__serial_digit_done:
        ret


; -------------------------------------------------------- available ---
; uint8_t serial_available()
;   Returns 1 in r24 if a received byte is waiting, 0 if not. Polled: RXC0 in
;   UCSR0A is set for as long as the receive register holds an unread byte.
;   There is no buffer behind this, so it never reports more than one.
serial_available:
        ldi     r24, 0
        ldi     r25, 0
        lds     r18, 0xC0               ; UCSR0A
        sbrc    r18, 7                  ; RXC0
        ldi     r24, 1
        ret


; ------------------------------------------------------------- read ---
; int16_t serial_read()
;   Returns the next received byte in r24:r25, or -1 when nothing is waiting.
;   Reading UDR0 is what clears RXC0.
serial_read:
        ldi     r24, 0xFF               ; -1
        ldi     r25, 0xFF
        lds     r18, 0xC0               ; UCSR0A
        sbrs    r18, 7                  ; RXC0
        ret
        lds     r24, 0xC6               ; UDR0
        ldi     r25, 0
        ret


; ------------------------------------------------------------ flush ---
; void serial_flush()
;   Returns once the last character has left the shift register, so a sketch
;   can be sure the wire is idle before it resets or sleeps. UDRE0 says the
;   holding register is free; TXC0 says the shifter is empty as well. TXC0 is
;   cleared by writing a one to it, which is done here so the next flush waits
;   on the next transmission rather than on this one.
serial_flush:
        lds     r18, 0xC0               ; UCSR0A
        sbrs    r18, 5                  ; UDRE0
        rjmp    serial_flush
        sbrs    r18, 6                  ; TXC0
        rjmp    serial_flush
        ldi     r18, 0x40
        sts     0xC0, r18               ; write 1 to TXC0 to clear it
        ret


; ARDIO_FALLBACK_BEGIN
; Nothing below this marker is needed to assemble the file -- every label it
; uses is defined above. The marker is here so the build's fallback-stripping
; pass finds the same structure it finds in the other runtime sources, and so
; anything added later that only exists to make this file stand alone has an
; obvious place to go.
