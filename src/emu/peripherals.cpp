// The emulated peripherals: I/O ports, timer 0, USART0 and the ADC.
//
// These are written from the same datasheet chapters the runtime assembly is,
// and deliberately model the awkward parts rather than the convenient ones. A
// peripheral that is nearly right is worse than one that is absent: an absent
// peripheral makes a sketch fail immediately and visibly, while a nearly right
// one produces a run that looks plausible and is wrong about timing.
//
// Two behaviours are worth naming up front because they are the ones a
// simplified model gets wrong:
//
//   * PORTx is not the pin. On an input bit it is the pull-up switch, and a
//     pin's level then comes from whatever is wired to it. Collapsing the two
//     makes it impossible to tell a sketch enabling a pull-up from a sketch
//     driving the pin high, which is precisely the distinction a button needs.
//
//   * Writing a 1 to a bit of PINx toggles that bit of PORTx. This is not a
//     typo in the datasheet and it is not rare -- it is the cheapest way to
//     flip an output on this part, so compilers and hand-written code both
//     emit it, and a model that treats PINx as read-only silently ignores
//     every one of those writes.
//
// On next_event, the choice made throughout: where the exact next moment of
// interest is uncertain, these return the EARLIER time. An early return costs
// the core a wakeup that finds nothing to do; a late one loses an interrupt or
// reports a stale flag to a polling sketch. Wasted wakeups are a slower
// emulator, missed events are a wrong one, so every rounding here goes toward
// waking up too often. Concretely: timer 0 schedules a wakeup for every flag
// it could set, including flags whose interrupt is not enabled, because a
// sketch polling TIFR0 must see the flag at the cycle the hardware would.

#include "ardio/emu/peripherals.h"

#include <algorithm>
#include <cmath>

namespace ardio::emu {
namespace {

constexpr uint64_t kNever = UINT64_MAX;

// Distance in ticks from position `from` to the next arrival at `to`, on a
// counter of `period` positions. Never zero: standing on a position is not
// arriving at it, and the flag is set by the arrival.
uint32_t ticks_until(uint32_t from, uint32_t to, uint32_t period) {
    uint32_t d = (to + period - from % period) % period;
    return d == 0 ? period : d;
}

} // namespace

// ================================================================= GPIO ====

GpioPort::GpioPort(uint16_t pin_reg, char label) : pin_reg_(pin_reg), label_(label) {}

bool GpioPort::claims(uint16_t addr) const {
    return addr >= pin_reg_ && addr <= uint16_t(pin_reg_ + 2);
}

uint8_t GpioPort::resolve_pins() const {
    uint8_t v = 0;
    for (uint8_t b = 0; b < 8; ++b) {
        bool level;
        if (external_[b] != PinState::Floating) {
            // Something outside is driving. It wins over the pull-up, and it
            // wins over the output driver too: shorting an output to a rail is
            // an electrical fault, and reporting what the pin is actually at
            // is more useful than pretending the conflict cannot happen.
            level = external_[b] == PinState::High;
        } else if (ddr_ & (1u << b)) {
            level = (port_ & (1u << b)) != 0;
        } else {
            // An input with nothing on it. With the pull-up on it reads high;
            // without, it is floating, and real silicon returns noise. This
            // returns 0 instead, because a floating read is a bug in the
            // sketch and a reproducible wrong answer can be debugged while a
            // random one cannot.
            level = (port_ & (1u << b)) != 0;
        }
        if (level) v |= uint8_t(1u << b);
    }
    return v;
}

uint8_t GpioPort::read(uint16_t addr) {
    if (addr == pin_reg_) return resolve_pins();
    if (addr == uint16_t(pin_reg_ + 1)) return ddr_;
    return port_;
}

void GpioPort::write(uint16_t addr, uint8_t value) {
    if (addr == pin_reg_) {
        // Writing PINx toggles PORTx, one bit per 1 written.
        port_ ^= value;
        return;
    }
    if (addr == uint16_t(pin_reg_ + 1)) {
        ddr_ = value;
        return;
    }
    port_ = value;
}

void GpioPort::advance(uint64_t) {}

PinState GpioPort::drive(uint8_t bit) const {
    if (bit > 7) return PinState::Floating;
    if (!(ddr_ & (1u << bit))) return PinState::Floating;
    return (port_ & (1u << bit)) ? PinState::High : PinState::Low;
}

bool GpioPort::pullup(uint8_t bit) const {
    if (bit > 7) return false;
    return !(ddr_ & (1u << bit)) && (port_ & (1u << bit));
}

void GpioPort::set_external(uint8_t bit, PinState state) {
    if (bit > 7) return;
    external_[bit] = state;
}

PinState GpioPort::external(uint8_t bit) const {
    return bit > 7 ? PinState::Floating : external_[bit];
}

// ============================================================== timer 0 ====

Timer0::Timer0(const avr::AvrDevice& device) {
    tccr0a_ = device.tccr0a;
    tccr0b_ = device.tccr0b;
    tcnt0_  = device.tcnt0;
    timsk0_ = device.timsk0;
    tifr0_  = device.tifr0;

    // OCR0A and OCR0B are not in the device table, but on every part that has
    // the TCCR0A/TCCR0B pair they sit immediately above TCNT0, in that order.
    // The exception is the ATmega8, whose timer 0 has no output compare at
    // all and no TCCR0A -- so the same condition that makes those registers
    // exist is the one that makes this derivation hold.
    ocr0a_ = uint16_t(tcnt0_ + 1);
    ocr0b_ = uint16_t(tcnt0_ + 2);

    vec_ovf_ = device.timer0_ovf_vector;
    // The three timer 0 vectors are always adjacent and always in the order
    // COMPA, COMPB, OVF, on every part in the table. Only the overflow number
    // is recorded, so the other two come off it.
    vec_compa_ = uint8_t(vec_ovf_ >= 2 ? vec_ovf_ - 2 : 0);
    vec_compb_ = uint8_t(vec_ovf_ >= 1 ? vec_ovf_ - 1 : 0);
}

bool Timer0::claims(uint16_t addr) const {
    return addr == tccr0a_ || addr == tccr0b_ || addr == tcnt0_ ||
           addr == ocr0a_ || addr == ocr0b_ || addr == timsk0_ || addr == tifr0_;
}

uint32_t Timer0::prescaler() const {
    switch (tccr0b_v_ & 0x07) {
        case 1: return 1;
        case 2: return 8;
        case 3: return 64;
        case 4: return 256;
        case 5: return 1024;
        // 6 and 7 clock the timer off the T0 pin. Nothing here drives T0, so
        // an external clock source means the counter does not advance -- which
        // is what a real part with a static T0 pin does too.
        default: return 0;
    }
}

bool Timer0::phase_correct() const {
    // WGM01:0 are TCCR0A bits 1:0, WGM02 is TCCR0B bit 3. Modes 1 and 5 are
    // the phase-correct ones.
    uint8_t wgm = uint8_t((tccr0a_v_ & 0x03) | ((tccr0b_v_ & 0x08) >> 1));
    return wgm == 1 || wgm == 5;
}

uint32_t Timer0::top() const {
    uint8_t wgm = uint8_t((tccr0a_v_ & 0x03) | ((tccr0b_v_ & 0x08) >> 1));
    // Modes 2 (CTC), 5 (phase-correct with OCR0A as TOP) and 7 (fast PWM with
    // OCR0A as TOP) take TOP from OCR0A; the rest count the full 8 bits.
    if (wgm == 2 || wgm == 5 || wgm == 7) return ocr0a_v_;
    return 0xFF;
}

uint32_t Timer0::period() const {
    uint32_t t = top();
    if (phase_correct()) {
        // Up to TOP and back down again, so a full cycle is 2*TOP positions.
        // A TOP of 0 leaves the counter stuck at 0; call the period 1 so the
        // arithmetic below has something to divide by.
        return t == 0 ? 1 : 2 * t;
    }
    return t + 1;
}

uint32_t Timer0::count_from_position() const {
    uint32_t t = top();
    if (!phase_correct()) return position_;
    return position_ <= t ? position_ : 2 * t - position_;
}

void Timer0::catch_up(uint64_t cycles) {
    uint32_t presc = prescaler();
    if (presc == 0 || cycles <= now_) {
        now_ = std::max(now_, cycles);
        return;
    }

    uint64_t total = uint64_t(tick_debt_) + (cycles - now_);
    uint64_t ticks = total / presc;
    tick_debt_ = uint32_t(total % presc);
    now_ = cycles;
    if (ticks == 0) return;

    uint32_t per = period();
    if (position_ >= per) {
        // TCNT0 (or a shrunken TOP) left the counter above TOP. Real silicon
        // does not snap it back: it keeps counting to 0xFF and wraps there,
        // one long cycle, before normal operation resumes.
        uint32_t to_wrap = 0x100 - position_;
        if (ticks < to_wrap) {
            position_ += uint32_t(ticks);
            return;
        }
        ticks -= to_wrap;
        position_ = 0;
        tifr_ |= 0x01;
        if (ticks == 0) return;
    }

    // Which positions set which flag. Overflow is the arrival at 0 in every
    // mode covered here: counting up it is the wrap past TOP, and in
    // phase-correct mode the flag is specified at BOTTOM, which is position 0
    // as well.
    uint32_t t = top();
    bool pc = phase_correct();

    auto crossed = [&](uint32_t target) {
        return uint64_t(ticks_until(position_, target % per, per)) <= ticks;
    };

    if (crossed(0)) tifr_ |= 0x01;
    if (ocr0a_v_ <= t) {
        if (crossed(ocr0a_v_)) tifr_ |= 0x02;
        if (pc && crossed((2 * t - ocr0a_v_) % per)) tifr_ |= 0x02;
    }
    if (ocr0b_v_ <= t) {
        if (crossed(ocr0b_v_)) tifr_ |= 0x04;
        if (pc && crossed((2 * t - ocr0b_v_) % per)) tifr_ |= 0x04;
    }

    position_ = uint32_t((position_ + ticks) % per);
}

void Timer0::advance(uint64_t cycles) { catch_up(cycles); }

uint8_t Timer0::read(uint16_t addr) {
    if (addr == tccr0a_) return tccr0a_v_;
    if (addr == tccr0b_) return tccr0b_v_;
    if (addr == tcnt0_) return uint8_t(count_from_position());
    if (addr == ocr0a_) return ocr0a_v_;
    if (addr == ocr0b_) return ocr0b_v_;
    if (addr == timsk0_) return timsk0_v_;
    if (addr == tifr0_) return tifr_;
    return 0;
}

void Timer0::write(uint16_t addr, uint8_t value) {
    if (addr == tccr0a_) { tccr0a_v_ = value; return; }
    if (addr == tccr0b_) {
        // A prescaler change restarts the divider chain from a known point.
        // Keeping the old fraction would carry up to 1023 cycles of the
        // previous scale into the new one.
        if ((tccr0b_v_ & 0x07) != (value & 0x07)) tick_debt_ = 0;
        tccr0b_v_ = value;
        return;
    }
    if (addr == tcnt0_) { position_ = value; return; }
    if (addr == ocr0a_) { ocr0a_v_ = value; return; }
    if (addr == ocr0b_) { ocr0b_v_ = value; return; }
    if (addr == timsk0_) { timsk0_v_ = value; return; }
    if (addr == tifr0_) {
        // A flag register is cleared by writing a ONE to the flag, not a zero.
        // Clearing it with a plain store of 0 -- the intuitive thing -- leaves
        // every flag exactly as it was.
        tifr_ = uint8_t(tifr_ & ~value);
        return;
    }
}

uint64_t Timer0::next_event() const {
    uint32_t presc = prescaler();
    if (presc == 0) return kNever;

    uint32_t per = period();
    uint32_t t = top();
    bool pc = phase_correct();

    uint64_t best = kNever;
    auto consider = [&](uint32_t target) {
        uint64_t d = ticks_until(position_, target % per, per);
        best = std::min(best, d);
    };

    if (position_ >= per) {
        // Mid-cycle above TOP: the next thing that happens is the wrap at 0xFF.
        best = 0x100 - position_;
    } else {
        consider(0);
        // Compare matches are scheduled whether or not their interrupt is
        // enabled, because TIFR0 is polled as often as it is used as an
        // interrupt source, and a poll must see the flag on the right cycle.
        if (ocr0a_v_ <= t) {
            consider(ocr0a_v_);
            if (pc) consider((2 * t - ocr0a_v_) % per);
        }
        if (ocr0b_v_ <= t) {
            consider(ocr0b_v_);
            if (pc) consider((2 * t - ocr0b_v_) % per);
        }
    }

    if (best == kNever) return kNever;
    // The k-th tick from now lands k prescaler cycles on, less whatever has
    // already accumulated toward the next one.
    return now_ + best * presc - tick_debt_;
}

uint8_t Timer0::pending_interrupt() const {
    // Lowest vector number first: that is the hardware's own priority order.
    if ((tifr_ & 0x02) && (timsk0_v_ & 0x02)) return vec_compa_;
    if ((tifr_ & 0x04) && (timsk0_v_ & 0x04)) return vec_compb_;
    if ((tifr_ & 0x01) && (timsk0_v_ & 0x01)) return vec_ovf_;
    return 0;
}

void Timer0::acknowledge_interrupt() {
    // Entering the vector clears the flag that caused it, and only that one.
    if ((tifr_ & 0x02) && (timsk0_v_ & 0x02)) { tifr_ &= uint8_t(~0x02); return; }
    if ((tifr_ & 0x04) && (timsk0_v_ & 0x04)) { tifr_ &= uint8_t(~0x04); return; }
    if ((tifr_ & 0x01) && (timsk0_v_ & 0x01)) { tifr_ &= uint8_t(~0x01); return; }
}

// =============================================================== USART0 ====

namespace {
// UCSRA
constexpr uint8_t kRxc  = 0x80;
constexpr uint8_t kTxc  = 0x40;
constexpr uint8_t kUdre = 0x20;
constexpr uint8_t kDor  = 0x08;
constexpr uint8_t kU2x  = 0x02;
// UCSRB
constexpr uint8_t kRxcie = 0x80;
constexpr uint8_t kTxcie = 0x40;
constexpr uint8_t kUdrie = 0x20;
constexpr uint8_t kRxen  = 0x10;
constexpr uint8_t kTxen  = 0x08;
constexpr uint8_t kUcsz2 = 0x04;
} // namespace

Usart0::Usart0(const avr::AvrDevice& device) {
    ucsra_ = device.ucsra;
    ucsrb_ = device.ucsrb;
    ucsrc_ = device.ucsrc;
    ubrrl_ = device.ubrrl;
    ubrrh_ = device.ubrrh;
    udr_   = device.udr;

    // The USART vectors are not in the device table. On every part in it they
    // follow the timer 0 overflow vector at a fixed distance, in the order RX,
    // UDRE, TX, so they are taken from there rather than written as 328P
    // literals. If a part ever lands in the table where that spacing does not
    // hold, the table needs three more fields; there is no way to detect the
    // mismatch from here.
    uint8_t base = device.timer0_ovf_vector;
    vec_rx_   = uint8_t(base ? base + 2 : 0);
    vec_udre_ = uint8_t(base ? base + 3 : 0);
    vec_tx_   = uint8_t(base ? base + 4 : 0);
}

bool Usart0::claims(uint16_t addr) const {
    return addr == ucsra_ || addr == ucsrb_ || addr == ucsrc_ ||
           addr == ubrrl_ || addr == ubrrh_ || addr == udr_;
}

uint32_t Usart0::bits_per_frame() const {
    uint8_t sz = uint8_t(((ucsrc_v_ >> 1) & 0x03) | ((ucsrb_v_ & kUcsz2) ? 0x04 : 0));
    uint32_t data = sz == 7 ? 9 : 5u + sz;
    uint32_t parity = (ucsrc_v_ & 0x20) ? 1 : 0;   // UPM1: even or odd parity
    uint32_t stop = (ucsrc_v_ & 0x08) ? 2 : 1;     // USBS
    return 1 + data + parity + stop;               // the start bit is always there
}

uint64_t Usart0::frame_cycles() const {
    // One bit lasts 16 clock cycles per divisor step in normal mode, 8 in
    // double-speed. That is the same relation serial_begin's UBRR arithmetic
    // is the inverse of, so a sketch that computes its divisor with the usual
    // F_CPU/(16*baud) - 1 gets exactly its baud rate back here.
    uint64_t per_bit = uint64_t(ubrr_v_ + 1) * ((ucsra_v_ & kU2x) ? 8 : 16);
    return per_bit * bits_per_frame();
}

void Usart0::feed(uint8_t byte) { rx_wire_.push_back(byte); start_next_rx(); }

void Usart0::feed(const std::string& bytes) {
    for (char c : bytes) rx_wire_.push_back(uint8_t(c));
    start_next_rx();
}

void Usart0::start_next_rx() {
    if (rx_active_ || rx_wire_.empty()) return;
    if (!(ucsrb_v_ & kRxen)) return;   // receiver off: the line is ignored
    rx_shift_ = rx_wire_.front();
    rx_wire_.pop_front();
    rx_active_ = true;
    rx_done_ = now_ + frame_cycles();
}

uint8_t Usart0::read(uint16_t addr) {
    if (addr == ucsra_) return ucsra_v_;
    if (addr == ucsrb_) return ucsrb_v_;
    if (addr == ucsrc_) return ucsrc_v_;
    if (addr == ubrrl_) return uint8_t(ubrr_v_ & 0xFF);
    if (addr == ubrrh_) return uint8_t(ubrr_v_ >> 8);
    if (addr == udr_) {
        uint8_t v = rx_data_;
        // Reading the data register is what acknowledges a received byte, so
        // RXC clears here and nowhere else.
        ucsra_v_ &= uint8_t(~kRxc);
        start_next_rx();
        return v;
    }
    return 0;
}

void Usart0::write(uint16_t addr, uint8_t value) {
    if (addr == ucsra_) {
        // RXC, UDRE, FE, DOR and UPE are read-only; TXC is cleared by writing
        // a one to it, and only U2X and MPCM are ordinary writable bits.
        if (value & kTxc) ucsra_v_ &= uint8_t(~kTxc);
        ucsra_v_ = uint8_t((ucsra_v_ & ~0x03) | (value & 0x03));
        return;
    }
    if (addr == ucsrb_) {
        ucsrb_v_ = value;
        if (!(value & kRxen)) {
            // Disabling the receiver flushes it: a byte half-shifted in is
            // gone, not held until the receiver comes back.
            rx_active_ = false;
            ucsra_v_ &= uint8_t(~kRxc);
        } else {
            start_next_rx();
        }
        return;
    }
    if (addr == ucsrc_) { ucsrc_v_ = value; return; }
    if (addr == ubrrl_) { ubrr_v_ = uint16_t((ubrr_v_ & 0xFF00) | value); return; }
    if (addr == ubrrh_) { ubrr_v_ = uint16_t((ubrr_v_ & 0x00FF) | (uint16_t(value & 0x0F) << 8)); return; }
    if (addr == udr_) {
        if (!(ucsrb_v_ & kTxen)) return;   // transmitter off: nothing goes out
        if (!tx_active_) {
            // The shift register is free, so the byte moves straight into it
            // and the data register is empty again immediately.
            tx_shift_ = value;
            tx_active_ = true;
            tx_done_ = now_ + frame_cycles();
            ucsra_v_ |= kUdre;
            ucsra_v_ &= uint8_t(~kTxc);
        } else {
            // Writing while busy is what a sketch that ignores UDRE does. The
            // byte is held, and UDRE goes low until the shift register frees.
            tx_buffer_ = value;
            tx_buffered_ = true;
            ucsra_v_ &= uint8_t(~kUdre);
        }
        return;
    }
}

void Usart0::advance(uint64_t cycles) {
    if (cycles < now_) return;
    // Several frames can complete inside one span, and a completed transmit
    // starts the buffered one, so this loops rather than checking once.
    for (;;) {
        uint64_t next = kNever;
        if (tx_active_) next = std::min(next, tx_done_);
        if (rx_active_) next = std::min(next, rx_done_);
        if (next == kNever || next > cycles) break;

        now_ = next;
        if (tx_active_ && tx_done_ == next) {
            output_.push_back(char(tx_shift_));
            tx_active_ = false;
            if (tx_buffered_) {
                tx_shift_ = tx_buffer_;
                tx_buffered_ = false;
                tx_active_ = true;
                tx_done_ = now_ + frame_cycles();
                ucsra_v_ |= kUdre;
            } else {
                // Nothing left anywhere: the transmission is complete, which
                // is the distinction TXC carries and UDRE does not.
                ucsra_v_ |= kTxc;
            }
        }
        if (rx_active_ && rx_done_ == next) {
            rx_active_ = false;
            if (ucsra_v_ & kRxc) {
                // The previous byte was never read. On real silicon the
                // receive FIFO overruns and the byte is lost -- reported, not
                // hidden, because a sketch that misses serial input wants to
                // know it missed it.
                ucsra_v_ |= kDor;
            } else {
                rx_data_ = rx_shift_;
                ucsra_v_ |= kRxc;
            }
            start_next_rx();
        }
    }
    now_ = cycles;
}

uint64_t Usart0::next_event() const {
    uint64_t next = kNever;
    if (tx_active_) next = std::min(next, tx_done_);
    if (rx_active_) next = std::min(next, rx_done_);
    return next;
}

uint8_t Usart0::pending_interrupt() const {
    if ((ucsra_v_ & kRxc) && (ucsrb_v_ & kRxcie)) return vec_rx_;
    if ((ucsra_v_ & kUdre) && (ucsrb_v_ & kUdrie)) return vec_udre_;
    if ((ucsra_v_ & kTxc) && (ucsrb_v_ & kTxcie)) return vec_tx_;
    return 0;
}

void Usart0::acknowledge_interrupt() {
    // Only TXC is cleared by taking its vector. The RX and UDRE interrupts are
    // level-triggered on a condition the handler must resolve by touching UDR,
    // and clearing them here would make a handler that forgets to read the
    // byte look correct.
    if ((ucsra_v_ & kRxc) && (ucsrb_v_ & kRxcie)) return;
    if ((ucsra_v_ & kUdre) && (ucsrb_v_ & kUdrie)) return;
    if ((ucsra_v_ & kTxc) && (ucsrb_v_ & kTxcie)) ucsra_v_ &= uint8_t(~kTxc);
}

// ================================================================== ADC ====

namespace {
constexpr uint8_t kAden = 0x80;
constexpr uint8_t kAdsc = 0x40;
constexpr uint8_t kAdif = 0x10;
constexpr uint8_t kAdie = 0x08;

// The channels that are not pins: the internal bandgap and ground. Nothing
// outside can drive these, so an injected voltage on them is ignored.
constexpr uint8_t kBandgapChannel = 14;
constexpr uint8_t kGroundChannel = 15;
constexpr double kBandgapVolts = 1.1;
} // namespace

Adc::Adc(const avr::AvrDevice& device) {
    admux_  = device.admux;
    adcsra_ = device.adcsra;
    adcsrb_ = device.adcsrb;
    adcl_   = device.adcl;
    adch_   = device.adch;

    // Same gap as the USART vectors: the table records only timer 0's. On the
    // 328P family the ADC vector is five past it. On the 2560 an analog
    // comparator vector sits in between, so that part needs a real field here
    // before its ADC interrupt can be emulated.
    vec_adc_ = uint8_t(device.timer0_ovf_vector ? device.timer0_ovf_vector + 5 : 0);
}

bool Adc::claims(uint16_t addr) const {
    return addr == admux_ || addr == adcsra_ || addr == adcsrb_ ||
           addr == adcl_ || addr == adch_;
}

uint32_t Adc::prescaler() const {
    uint8_t bits = uint8_t(adcsra_v_ & 0x07);
    // Division factors 2, 2, 4, 8, 16, 32, 64, 128 -- the first two settings
    // both mean 2, which is why this is not simply 1 << bits.
    return bits < 2 ? 2u : (1u << bits);
}

double Adc::reference_voltage() const {
    switch ((admux_v_ >> 6) & 0x03) {
        case 0: return aref_;            // AREF pin, whatever is on it
        case 1: return avcc_;            // AVCC
        case 3: return kBandgapVolts;    // internal 1.1 V
        default: return avcc_;           // setting 2 is reserved on this part
    }
}

uint16_t Adc::sample(uint8_t channel) const {
    double v;
    if (channel == kGroundChannel) v = 0.0;
    else if (channel == kBandgapChannel) v = kBandgapVolts;
    else v = channel_[channel & 0x0F];

    double vref = reference_voltage();
    if (vref <= 0.0) return 0;
    // The conversion is a ratio against the reference, and a channel above the
    // reference pins at full scale rather than wrapping.
    double raw = std::floor(v / vref * 1024.0);
    if (raw < 0.0) return 0;
    if (raw > 1023.0) return 1023;
    return uint16_t(raw);
}

void Adc::set_channel_voltage(uint8_t channel, double volts) {
    if (channel >= 16) return;
    channel_[channel] = volts;
}

double Adc::channel_voltage(uint8_t channel) const {
    return channel >= 16 ? 0.0 : channel_[channel];
}

uint8_t Adc::read(uint16_t addr) {
    if (addr == admux_) return admux_v_;
    if (addr == adcsra_) return adcsra_v_;
    if (addr == adcsrb_) return adcsrb_v_;
    if (addr == adcl_ || addr == adch_) {
        // ADLAR shifts the ten bits to the top of the pair, so a sketch that
        // only wants eight bits can read ADCH alone.
        uint16_t r = (admux_v_ & 0x20) ? uint16_t(result_ << 6) : result_;
        return addr == adcl_ ? uint8_t(r & 0xFF) : uint8_t(r >> 8);
    }
    return 0;
}

void Adc::write(uint16_t addr, uint8_t value) {
    if (addr == admux_) { admux_v_ = value; return; }
    if (addr == adcsrb_) { adcsrb_v_ = value; return; }
    if (addr == adcsra_) {
        if (value & kAdif) adcsra_v_ &= uint8_t(~kAdif);   // written as a one to clear
        adcsra_v_ = uint8_t((adcsra_v_ & kAdif) | (value & ~kAdif));

        if (!(adcsra_v_ & kAden)) {
            // Switching the ADC off abandons a conversion in progress, and the
            // next one after it is switched back on is a first conversion
            // again -- it has to re-initialise the analog circuitry.
            converting_ = false;
            first_conversion_ = true;
            adcsra_v_ &= uint8_t(~kAdsc);
            return;
        }
        if (converting_) {
            // ADSC reads as one for as long as the conversion runs, whatever
            // was written to it: it is cleared by the hardware, not by a
            // store. A sketch polling for it to fall is the normal way to wait
            // for a result.
            adcsra_v_ |= kAdsc;
            return;
        }
        if (value & kAdsc) {
            converting_ = true;
            convert_channel_ = uint8_t(admux_v_ & 0x0F);
            // 25 ADC clocks for the first conversion after the ADC is enabled,
            // 13 for each one after: the extra 12 are the initialisation of
            // the sample-and-hold, and a sketch that measures its own
            // analogRead will see that first one take twice as long.
            uint32_t clocks = first_conversion_ ? 25 : 13;
            convert_done_ = now_ + uint64_t(clocks) * prescaler();
        }
        return;
    }
}

void Adc::advance(uint64_t cycles) {
    if (cycles < now_) return;
    if (converting_ && convert_done_ <= cycles) {
        result_ = sample(convert_channel_);
        converting_ = false;
        first_conversion_ = false;
        adcsra_v_ &= uint8_t(~kAdsc);
        adcsra_v_ |= kAdif;
    }
    now_ = cycles;
}

uint64_t Adc::next_event() const { return converting_ ? convert_done_ : kNever; }

uint8_t Adc::pending_interrupt() const {
    return ((adcsra_v_ & kAdif) && (adcsra_v_ & kAdie)) ? vec_adc_ : 0;
}

void Adc::acknowledge_interrupt() { adcsra_v_ &= uint8_t(~kAdif); }

// ============================================================== factory ====

GpioPort* PeripheralSet::port_for(uint16_t pin_reg) const {
    for (GpioPort* p : ports)
        if (p->pin_reg() == pin_reg) return p;
    return nullptr;
}

PeripheralSet make_standard_peripherals(const avr::AvrDevice& device) {
    PeripheralSet set;

    // The ports are taken from the pin map rather than from a list of port
    // letters, because the pin map is the part of the description that says
    // which ports the part actually brought out.
    for (const avr::PinMapping& m : device.pins) {
        if (m.pin_reg == 0) continue;              // a number this board lacks
        if (set.port_for(m.pin_reg)) continue;
        // Ports run at a stride of three from PINA at 0x20, so the letter
        // falls out of the address.
        char label = char('A' + (m.pin_reg - 0x20) / 3);
        auto port = std::make_unique<GpioPort>(m.pin_reg, label);
        set.ports.push_back(port.get());
        set.owned.push_back(std::move(port));
    }

    // A device that does not describe a peripheral does not get one. Building
    // it anyway at address zero would give it a claim on r0.
    if (device.tcnt0 != 0) {
        auto t = std::make_unique<Timer0>(device);
        set.timer0 = t.get();
        set.owned.push_back(std::move(t));
    }
    if (device.udr != 0) {
        auto u = std::make_unique<Usart0>(device);
        set.usart0 = u.get();
        set.owned.push_back(std::move(u));
    }
    if (device.adcsra != 0) {
        auto a = std::make_unique<Adc>(device);
        set.adc = a.get();
        set.owned.push_back(std::move(a));
    }
    return set;
}

} // namespace ardio::emu
