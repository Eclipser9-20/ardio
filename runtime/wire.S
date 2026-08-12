; ardio runtime -- I2C (TWI) master, ATmega328P at 16 MHz.
;
; Written from the ATmega328P datasheet's two-wire serial interface chapter.
; The TWI control registers live above the 0x00-0x3F I/O window, so every
; access here is lds/sts against the data-space address rather than in/out:
;
;     TWBR 0xB8   bit-rate divider
;     TWSR 0xB9   status (top 5 bits) + prescaler select (low 2 bits)
;     TWAR 0xBA   slave address (unused in master mode)
;     TWDR 0xBB   data
;     TWCR 0xBC   control
;
; TWCR bits: TWINT 7, TWEA 6, TWSTA 5, TWSTO 4, TWWC 3, TWEN 2, TWIE 0.
; Every operation is started by writing TWINT back as a 1 (which clears it)
; together with TWEN, then polled until the hardware sets TWINT again.
;
; Calling convention is avr-gcc's: first argument in r24:r25, return value in
; r24:r25, r18-r27/r30/r31 free to clobber. Nothing here touches a call-saved
; register, so no prologue is needed.
;
; SRAM: this file keeps no state at all. (lcd.S owns 0x0300-0x0301,
; button.S owns 0x0302-0x0304.)

; ---------------------------------------------------------------------------
; i2c_init(void)
;
; SCL = F_CPU / (16 + 2*TWBR*prescaler). With F_CPU 16 MHz, prescaler 1 and
; TWBR 72 that is 16000000 / (16 + 144) = 100 kHz exactly.
; ---------------------------------------------------------------------------
i2c_init:
        ldi     r18, 72
        sts     0xB8, r18               ; TWBR
        ldi     r18, 0
        sts     0xB9, r18               ; TWSR: TWPS1:0 = 00 -> prescaler 1
        ldi     r18, 0x04
        sts     0xBC, r18               ; TWCR = TWEN, bus enabled and idle
        ret

; ---------------------------------------------------------------------------
; i2c_start(void)
;
; Transmits a START condition (a repeated START if the bus is already ours)
; and returns once the hardware has taken it.
; ---------------------------------------------------------------------------
i2c_start:
        ldi     r18, 0xA4               ; TWINT | TWSTA | TWEN
        sts     0xBC, r18
i2c_start_wait:
        lds     r18, 0xBC
        sbrs    r18, 7                  ; TWINT set means START is on the wire
        rjmp    i2c_start_wait
        ret

; ---------------------------------------------------------------------------
; i2c_stop(void)
;
; Transmits a STOP condition. TWSTO is cleared by hardware once the bus is
; released, so waiting on it keeps a back-to-back start/stop pair legal.
; ---------------------------------------------------------------------------
i2c_stop:
        ldi     r18, 0x94               ; TWINT | TWSTO | TWEN
        sts     0xBC, r18
i2c_stop_wait:
        lds     r18, 0xBC
        sbrc    r18, 4                  ; TWSTO still set -> STOP in progress
        rjmp    i2c_stop_wait
        ret

; ---------------------------------------------------------------------------
; uint8_t i2c_write(uint8_t byte)
;
; Shifts one byte out and returns the masked TWSR status code, so the caller
; can tell 0x18 (SLA+W acked) or 0x28 (data acked) from 0x20/0x30 (nacked).
; Argument in r24, result in r24:r25.
; ---------------------------------------------------------------------------
i2c_write:
        sts     0xBB, r24               ; TWDR
        ldi     r18, 0x84               ; TWINT | TWEN -> start shifting
        sts     0xBC, r18
i2c_write_wait:
        lds     r18, 0xBC
        sbrs    r18, 7
        rjmp    i2c_write_wait
        lds     r24, 0xB9               ; TWSR
        andi    r24, 0xF8               ; drop the prescaler bits
        ldi     r25, 0
        ret
