; ardio analog input for the ATmega328P.
;
; Written from the ATmega328P datasheet (the Analog-to-Digital Converter
; chapter and the register summary) and assembled by ardio's own assembler --
; no external toolchain, no third-party sources.
;
; Calling convention matches avr-gcc:
;   * first int argument in r24:r25
;   * a 16-bit result comes back in r24:r25, low byte in r24
;   * r18-r27, r30, r31 are call-clobbered; r2-r17, r28, r29 are preserved
;
; Register addresses (datasheet register summary). All four live above 0x5F,
; outside the in/out I/O window, so every access is lds/sts.
;
;       ADCL 0x78   ADCH 0x79   ADCSRA 0x7A   ADMUX 0x7C

.equ    ADCL,           0x78
.equ    ADCH,           0x79
.equ    ADCSRA,         0x7A
.equ    ADMUX,          0x7C

; ADMUX: REFS1:0 select the reference, MUX3:0 the input channel. REFS0 alone
; is AVcc with an external capacitor on AREF -- the 5 V supply, which is what
; a joystick or a potentiometer wired across the board's own rails is being
; measured against. ADLAR stays clear so the result is right-adjusted and the
; ten bits read back as a plain 0..1023 integer.
.equ    ADMUX_AVCC,     0x40            ; REFS0
.equ    ADMUX_CHANNEL,  0x0F            ; MUX3:0

; ADCSRA: ADEN enables the converter, ADSC starts a conversion and stays set
; until that conversion finishes. ADPS2:0 = 111 divides the 16 MHz system
; clock by 128, giving a 125 kHz ADC clock -- inside the 50-200 kHz window the
; datasheet requires for the full ten bits of resolution. A conversion takes
; 13 ADC clocks, so about 104 us.
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
        sts     ADMUX, r24              ; AVcc reference, channel 0, no ADLAR
        ldi     r24, ADCSRA_ADEN|ADCSRA_DIV128
        sts     ADCSRA, r24             ; enable, ADC clock = 16 MHz / 128
        ret


; --------------------------------------------------------- analog_read ---
; uint16_t analog_read(uint8_t channel)
;   channel in r24 (0-7 for the ADC0-ADC7 pins), result 0-1023 in r24:r25
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
        andi    r24, ADMUX_CHANNEL      ; only MUX3:0 belong to the caller
        ori     r24, ADMUX_AVCC         ; keep AVcc as the reference
        sts     ADMUX, r24

        lds     r24, ADCSRA
        ori     r24, ADCSRA_ADSC
        sts     ADCSRA, r24             ; start a single conversion
__ardio_adc_wait:
        lds     r24, ADCSRA
        andi    r24, ADCSRA_ADSC        ; ADSC stays set while converting
        brne    __ardio_adc_wait

        lds     r24, ADCL               ; low byte first: this locks the pair
        lds     r25, ADCH               ; high byte releases it again
        ret


; ---------------------------------------------------------- analogRead ---
; uint16_t analogRead(uint8_t pin)
;
; The Arduino spelling, taking a pin number rather than a channel. ardio's pin
; map (see runtime/core.S) gives A0-A5 the numbers 14-19, and sketches also
; pass the bare channel numbers 0-5, so anything from 14 up is folded back
; down to a channel and anything below is already one.
analogRead:
        cpi     r24, 14
        brlo    analog_read
        subi    r24, 14
        rjmp    analog_read
