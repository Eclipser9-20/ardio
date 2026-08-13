; ardio runtime -- I2C (TWI) master.
;
; Written from the megaAVR datasheets' two-wire serial interface chapter. The
; four registers it needs are named rather than numbered -- AD_TWBR is the
; bit-rate divider, AD_TWSR the status byte whose top five bits carry the state
; and whose low two select the prescaler, AD_TWDR the data register and AD_TWCR
; the control register -- because the block sits at 0xB8 on the ATmega328P and
; at 0x20 on the ATmega8. Every access is lds/sts, which reaches either.
;
; (TWAR, the slave address, is not used: this is a master-only driver.)
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
; button.S owns 0x0308-0x0317.)

; ---------------------------------------------------------------------------
; i2c_init(void)
;
; SCL = F_CPU / (16 + 2*TWBR*prescaler), so with the prescaler at 1 a 100 kHz
; bus wants TWBR = (F_CPU / 100000 - 16) / 2 -- 72 at 16 MHz, 32 at 8 MHz.
; ---------------------------------------------------------------------------
i2c_init:
        ldi     r18, (AD_F_CPU / 100000 - 16) / 2
        sts     AD_TWBR, r18
        ldi     r18, 0
        sts     AD_TWSR, r18            ; TWPS1:0 = 00 -> prescaler 1
        ldi     r18, 0x04
        sts     AD_TWCR, r18            ; TWEN: bus enabled and idle
        ret

; ---------------------------------------------------------------------------
; i2c_start(void)
;
; Transmits a START condition (a repeated START if the bus is already ours)
; and returns once the hardware has taken it.
; ---------------------------------------------------------------------------
i2c_start:
        ldi     r18, 0xA4               ; TWINT | TWSTA | TWEN
        sts     AD_TWCR, r18
i2c_start_wait:
        lds     r18, AD_TWCR
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
        sts     AD_TWCR, r18
i2c_stop_wait:
        lds     r18, AD_TWCR
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
        sts     AD_TWDR, r24
        ldi     r18, 0x84               ; TWINT | TWEN -> start shifting
        sts     AD_TWCR, r18
i2c_write_wait:
        lds     r18, AD_TWCR
        sbrs    r18, 7
        rjmp    i2c_write_wait
        lds     r24, AD_TWSR            ; TWSR
        andi    r24, 0xF8               ; drop the prescaler bits
        ldi     r25, 0
        ret
