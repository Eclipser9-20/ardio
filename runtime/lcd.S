; ardio runtime -- HD44780 16x2 character LCD behind a PCF8574 I2C expander.
;
; Written from the HD44780U and PCF8574 datasheets. The expander is a plain
; 8-bit output latch: one I2C write puts one byte on P0-P7, and the LCD is
; wired to those pins in the arrangement the Label Maker uses:
;
;     P0 RS    0 = instruction register, 1 = data register
;     P1 RW    tied low here -- this driver never reads the busy flag
;     P2 EN    falling edge latches the nibble
;     P3 LED   backlight
;     P4-P7    D4-D7, the high nibble of the 4-bit bus
;
; So each byte sent to the display is two expander writes per nibble edge:
; value, value|EN, value. Since RW stays low we cannot poll the busy flag, so
; every operation is followed by a fixed delay taken from the datasheet's
; execution times (37 us for most instructions, 1.52 ms for clear/home).
;
; ardio's assembler produces one flat image per source file and there is no
; linker, so this file carries its own small copy of the TWI sequence rather
; than calling into wire.S. wire.S remains the general-purpose public API;
; the code below is the private, single-purpose version of it.
;
; SRAM state (documented so other runtime files do not collide):
;     0x0300  7-bit I2C address of the expander
;     0x0301  backlight/control shadow, OR'd into every expander write
; (button.S owns 0x0308-0x0317. wire.S owns nothing.)
;
; Registers: avr-gcc convention, args r24:r25 then r22:r23. r16 is used as the
; expander-byte accumulator and is call-saved, so it is pushed by the public
; entry points that touch it.

; ---------------------------------------------------------------------------
; lcd_pcf_send  (internal) -- r16 = byte for the expander.
;
; Full START / SLA+W / data / STOP transaction. Clobbers r18 only, so r16,
; r24 and r25 survive across a call.
; ---------------------------------------------------------------------------
lcd_pcf_send:
        lds     r18, 0x0301             ; backlight shadow
        or      r16, r18                ; every write carries it
        ldi     r18, 0xA4               ; TWINT | TWSTA | TWEN
        sts     AD_TWCR, r18
lcd_tw_start:
        lds     r18, AD_TWCR
        sbrs    r18, 7
        rjmp    lcd_tw_start
        lds     r18, 0x0300             ; 7-bit address
        lsl     r18                     ; shift up, bit 0 = 0 -> write
        sts     AD_TWDR, r18
        ldi     r18, 0x84               ; TWINT | TWEN
        sts     AD_TWCR, r18
lcd_tw_addr:
        lds     r18, AD_TWCR
        sbrs    r18, 7
        rjmp    lcd_tw_addr
        sts     AD_TWDR, r16            ; the payload byte
        ldi     r18, 0x84
        sts     AD_TWCR, r18
lcd_tw_data:
        lds     r18, AD_TWCR
        sbrs    r18, 7
        rjmp    lcd_tw_data
        ldi     r18, 0x94               ; TWINT | TWSTO | TWEN
        sts     AD_TWCR, r18
lcd_tw_stop:
        lds     r18, AD_TWCR
        sbrc    r18, 4
        rjmp    lcd_tw_stop
        ret

; ---------------------------------------------------------------------------
; lcd_delay_short (internal) -- about 40 us at 16 MHz.
;
; 213 iterations of dec+brne is 639 cycles, ~40 us, which covers the 37 us
; execution time of every instruction except clear and return-home.
; ---------------------------------------------------------------------------
lcd_delay_short:
        ldi     r20, 213
lcd_delay_short_1:
        dec     r20
        brne    lcd_delay_short_1
        ret

; ---------------------------------------------------------------------------
; lcd_delay_ms (internal) -- r21 = milliseconds. 21 * 250 * 3 ~= 15.8k cycles
; per outer pass, i.e. ~1 ms at 16 MHz.
; ---------------------------------------------------------------------------
lcd_delay_ms:
        ldi     r19, 21
lcd_delay_ms_1:
        ldi     r20, 250
lcd_delay_ms_2:
        dec     r20
        brne    lcd_delay_ms_2
        dec     r19
        brne    lcd_delay_ms_1
        dec     r21
        brne    lcd_delay_ms
        ret

; ---------------------------------------------------------------------------
; lcd_pulse (internal) -- r16 = data nibble in the high half plus RS.
;
; Drives EN high then low; the falling edge is what the controller latches on.
; ---------------------------------------------------------------------------
lcd_pulse:
        rcall   lcd_pcf_send            ; set up the nibble, EN low
        ori     r16, 0x04               ; EN high
        rcall   lcd_pcf_send
        rcall   lcd_delay_short
        andi    r16, 0xFB               ; EN low -> latch
        rcall   lcd_pcf_send
        rcall   lcd_delay_short
        ret

; ---------------------------------------------------------------------------
; lcd_send (internal) -- r24 = byte, r25 = RS (0 instruction, 1 data).
;
; High nibble first, then low, as the 4-bit interface requires.
; ---------------------------------------------------------------------------
lcd_send:
        push    r16
        mov     r16, r24
        andi    r16, 0xF0
        or      r16, r25
        rcall   lcd_pulse
        mov     r16, r24
        swap    r16
        andi    r16, 0xF0
        or      r16, r25
        rcall   lcd_pulse
        pop     r16
        ret

; ---------------------------------------------------------------------------
; lcd_init(uint8_t i2c_address)
;
; The datasheet's 4-bit initialisation-by-instruction sequence: wait for the
; supply to settle, send 0x3 three times as a lone nibble to force the
; controller into a known 8-bit state whatever it powered up in, then 0x2 to
; switch to 4 bits, after which whole bytes can be sent.
; ---------------------------------------------------------------------------
lcd_init:
        push    r16
        sts     0x0300, r24             ; remember the address
        ldi     r24, 0x08
        sts     0x0301, r24             ; backlight on by default

        ldi     r18, (AD_F_CPU / 100000 - 16) / 2   ; TWI at 100 kHz, prescaler 1
        sts     AD_TWBR, r18
        ldi     r18, 0
        sts     AD_TWSR, r18
        ldi     r18, 0x04
        sts     AD_TWCR, r18

        ldi     r21, 50                 ; >40 ms after power-on
        rcall   lcd_delay_ms

        ldi     r16, 0x30               ; function set, 8-bit, attempt 1
        rcall   lcd_pulse
        ldi     r21, 5                  ; >4.1 ms
        rcall   lcd_delay_ms
        ldi     r16, 0x30               ; attempt 2
        rcall   lcd_pulse
        ldi     r21, 1                  ; >100 us
        rcall   lcd_delay_ms
        ldi     r16, 0x30               ; attempt 3
        rcall   lcd_pulse
        ldi     r21, 1
        rcall   lcd_delay_ms
        ldi     r16, 0x20               ; now switch to the 4-bit interface
        rcall   lcd_pulse
        ldi     r21, 1
        rcall   lcd_delay_ms

        ldi     r25, 0                  ; from here on: whole bytes, RS = 0
        ldi     r24, 0x28               ; function set: 4-bit, 2 lines, 5x8
        rcall   lcd_send
        ldi     r24, 0x08               ; display off while we tidy up
        rcall   lcd_send
        ldi     r24, 0x01               ; clear
        rcall   lcd_send
        ldi     r21, 3                  ; clear needs 1.52 ms
        rcall   lcd_delay_ms
        ldi     r24, 0x06               ; entry mode: increment, no shift
        rcall   lcd_send
        ldi     r24, 0x0C               ; display on, cursor off, no blink
        rcall   lcd_send
        pop     r16
        ret

; ---------------------------------------------------------------------------
; lcd_clear(void) -- clears the display and homes the cursor.
; ---------------------------------------------------------------------------
lcd_clear:
        ldi     r24, 0x01
        ldi     r25, 0
        rcall   lcd_send
        ldi     r21, 3
        rcall   lcd_delay_ms
        ret

; ---------------------------------------------------------------------------
; lcd_set_cursor(uint8_t col, uint8_t row)
;
; DDRAM on a 16x2 module is one contiguous run per line: row 0 starts at 0x00
; and row 1 at 0x40. Bit 7 of the instruction selects "set DDRAM address".
; col in r24, row in r22.
; ---------------------------------------------------------------------------
lcd_set_cursor:
        andi    r24, 0x0F               ; 16 columns
        sbrc    r22, 0                  ; odd row -> second line
        ori     r24, 0x40
        ori     r24, 0x80               ; set-DDRAM-address opcode
        ldi     r25, 0
        rcall   lcd_send
        ret

; ---------------------------------------------------------------------------
; lcd_write_char(uint8_t c) -- writes one character at the cursor, which the
; entry mode above then advances.
; ---------------------------------------------------------------------------
lcd_write_char:
        ldi     r25, 1                  ; RS = 1 -> data register
        rcall   lcd_send
        ret

; ---------------------------------------------------------------------------
; lcd_backlight(uint8_t on)
;
; The backlight is just P3 of the expander, so this updates the shadow byte
; and pushes it out immediately with everything else idle.
; ---------------------------------------------------------------------------
lcd_backlight:
        cpi     r24, 0
        breq    lcd_backlight_off
        ldi     r24, 0x08
        rjmp    lcd_backlight_store
lcd_backlight_off:
        ldi     r24, 0
lcd_backlight_store:
        sts     0x0301, r24
        push    r16
        ldi     r16, 0                  ; the shadow is OR'd in by pcf_send
        rcall   lcd_pcf_send
        pop     r16
        ret
