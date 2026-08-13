; ardio analog input.
;
; Written from the megaAVR datasheets' Analog-to-Digital Converter chapter and
; assembled by ardio's own assembler -- no external toolchain, no third-party
; sources.
;
; Calling convention matches avr-gcc:
;   * first int argument in r24:r25
;   * a 16-bit result comes back in r24:r25, low byte in r24
;   * r0 is scratch, destroyed by any call
;   * r18-r27, r30, r31 are call-clobbered; r2-r17, r28, r29 are preserved
;
; The four ADC registers are named rather than numbered: AD_ADCL, AD_ADCH,
; AD_ADCSRA and AD_ADMUX come from the device description. Every access is
; lds/sts, which reaches any data-space address -- on the parts that put the
; ADC in low I/O space in/out would be a word shorter, but the assembler cannot
; tell from a symbolic address whether that form is legal, and a wrong guess
; would write to a different register entirely.

; ADMUX: REFS1:0 select the reference, MUX3:0 the input channel. REFS0 alone
; is AVcc with an external capacitor on AREF -- the 5 V supply, which is what
; a joystick or a potentiometer wired across the board's own rails is being
; measured against. ADLAR stays clear so the result is right-adjusted and the
; ten bits read back as a plain 0..1023 integer.
.equ    ADMUX_AVCC,     0x40            ; REFS0
.equ    ADMUX_CHANNEL,  0x0F            ; MUX3:0

; ADCSRA: ADEN enables the converter, ADSC starts a conversion and stays set
; until that conversion finishes. ADPS2:0 = 111 divides the system clock by
; 128, giving 125 kHz from 16 MHz -- inside the 50-200 kHz window the datasheet
; requires for the full ten bits of resolution. A conversion takes 13 ADC
; clocks, so about 104 us.
.equ    ADCSRA_ADEN,    0x80
.equ    ADCSRA_ADSC,    0x40
.equ    ADCSRA_DIV128,  0x07            ; ADPS2:0


; ----------------------------------------------------------- adc_init ---
; void adc_init(void)
;
; Powers up the converter, selects AVcc as the reference and sets the /128
; prescaler. Call once from setup() before the first analog_read.
adc_init:
        ldi     r24, ADMUX_AVCC
        sts     AD_ADMUX, r24           ; AVcc reference, channel 0, no ADLAR
        ldi     r24, ADCSRA_ADEN|ADCSRA_DIV128
        sts     AD_ADCSRA, r24          ; enable, ADC clock = F_CPU / 128
        ret


; --------------------------------------------------------- analog_read ---
; uint16_t analog_read(uint8_t input)
;   input in r24 -- the analog input number, the n in "An", not the raw ADC
;   channel. Result 0-1023 in r24:r25, or 0 for an input this part does not
;   have.
;
; The two are the same thing on the parts whose A0 is ADC0, and are not on the
; ones where the header is wired to the multiplexer in some other order (a
; Leonardo's A0 is ADC7). __ardio_analogmap holds the translation: one byte per
; analog input giving its channel, or 0xFF for a number the part does not
; offer. It is a flash table, so it is read with lpm, and the byte address of
; entry n is __ardio_analogmap * 2 + n because labels are word addresses.
;
; The channel goes into MUX3:0 while the reference bits are rewritten, then
; ADSC starts a single conversion and the loop waits for the hardware to clear
; it again.
;
; The read order at the end is not a matter of taste. The datasheet says ADCL
; must be read first: reading it locks the ADC data registers so the pair
; cannot be updated underneath the read, and reading ADCH releases them again.
; Take ADCH first and the lock never engages -- a conversion completing
; between the two reads swaps in a fresh low byte, and the two halves come
; from different samples. On a slowly moving signal that is invisible; on a
; joystick being flicked it is an occasional wild reading with no obvious
; cause. Low byte first, always.
analog_read:
        cpi     r24, AD_NUM_ANALOG
        brsh    __ardio_adc_none
        ldi     r18, 0                  ; a zero to carry into the high byte
        ldi     r30, lo8(__ardio_analogmap * 2)
        ldi     r31, hi8(__ardio_analogmap * 2)
        add     r30, r24
        adc     r31, r18
        lpm                             ; r0 = ADC channel for this input
        mov     r24, r0
        cpi     r24, 0xFF
        breq    __ardio_adc_none        ; no such analog input on this part

        andi    r24, ADMUX_CHANNEL      ; only MUX3:0 belong to the caller
        ori     r24, ADMUX_AVCC         ; keep AVcc as the reference
        sts     AD_ADMUX, r24

        lds     r24, AD_ADCSRA
        ori     r24, ADCSRA_ADSC
        sts     AD_ADCSRA, r24          ; start a single conversion
__ardio_adc_wait:
        lds     r24, AD_ADCSRA
        andi    r24, ADCSRA_ADSC        ; ADSC stays set while converting
        brne    __ardio_adc_wait

        lds     r24, AD_ADCL            ; low byte first: this locks the pair
        lds     r25, AD_ADCH            ; high byte releases it again
        ret
__ardio_adc_none:
        ldi     r24, 0
        ldi     r25, 0
        ret


; ---------------------------------------------------------- analogRead ---
; uint16_t analogRead(uint8_t pin)
;
; The Arduino spelling, which accepts either an analog input number or the
; digital pin number the same physical pin carries.
;
; The 14 below is the one part-specific number left in the runtime, and it is
; here because the device description does not carry the fact it stands for:
; the digital pin number of A0. AD_NUM_PINS - AD_NUM_ANALOG comes to 14 on most
; parts and would be the obvious substitute, but it is 12 on the ATmega328P,
; whose table lists eight analog inputs while only six of them have a digital
; pin at all (A6 and A7 are ADC-only pads). Folding pin 14 by 12 would select
; channel 2 and return a reading from the wrong pin -- a wrong answer with
; nothing to show for itself. Until the description carries A0's digital pin
; number this stays a literal, and stays wrong only on parts whose A0 is not
; pin 14, where today it is wrong anyway.
analogRead:
        cpi     r24, 14
        brlo    analog_read
        subi    r24, 14
        rjmp    analog_read
