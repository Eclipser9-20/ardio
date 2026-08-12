// IRremote.h — ardio's own NEC infrared receive class, API-compatible with the
// commonly used IRrecv sketch interface.
//
// The decoding lives in ardio's assembly runtime (runtime/ir.S), which samples
// the receiver module's output with cycle-counted timing loops and rebuilds a
// NEC frame from the published protocol timings. Nothing here is derived from
// any existing IR library.
//
// Wiring: a TSOP-style receiver module's OUT pin goes to any digital pin. Its
// output is active low — idle high, pulled low by a burst of carrier — and the
// runtime enables the internal pull-up on the pin.
//
// ---------------------------------------------------------------------------
// WHAT THE RUNTIME ACTUALLY PROVIDES
//
// runtime/ir.S exports six labels: ir_begin, ir_available, ir_read_command,
// ir_read_address, ir_resume and ir_decode. Every method below forwards to
// one of them with an inline body -- ardio has no linker, so a method with
// only a declaration fails at assembly time as "nothing implements
// 'IRrecv__decode'" rather than at compile time.
//
// Divergences from the published API:
//
//   * decode() BLOCKS. ir_decode waits for a frame and gives up after about
//     100 ms of silence, so a sketch calling it once round loop() polls the
//     remote at roughly 10 Hz in the worst case. The published API's decode()
//     returns immediately.
//   * The 32-bit frame is not kept whole. value carries the address in its
//     high byte and the command in its low byte, and the two bytes are also
//     readable on their own. A sketch comparing against a full 32-bit remote
//     code should compare getCommand() instead.
//   * decode(decode_results&) becomes decode_into(decode_results*): references
//     to class types are not usable in this subset.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_IRREMOTE_H
#define ARDIO_IRREMOTE_H

// Protocol identifier reported for a frame this runtime understands. Only NEC
// is decoded, so anything else is UNKNOWN.
const int UNKNOWN = 0;
const int NEC = 1;

// Bits in a NEC frame, for sketches that want to print it.
const int NEC_BITS = 32;

// ------------------------------------------------------------- runtime -----
//
// Entry points exported by runtime/ir.S as plain labels.

// Configure a pin as the receiver input, with its pull-up on.
void ir_begin(int pin);

// 1 once a complete frame has been decoded and not yet consumed by ir_resume.
int ir_available();

// The command and address bytes of the most recently decoded frame.
int ir_read_command();
int ir_read_address();

// Clear the available flag so the next frame can be reported. The stored
// address and command survive, so a repeat frame still has something to repeat.
void ir_resume();

// Block until a frame arrives, then decode it. Returns 1 on a good frame
// (including a repeat, which re-reports the previous code), 0 on timeout or a
// failed checksum. Times out after about 100 ms of silence.
int ir_decode();

// ------------------------------------------------------------- results -----

class decode_results {
public:
    decode_results() {
        value = 0;
        address = 0;
        command = 0;
        decode_type = UNKNOWN;
        bits = 0;
    }

    // Address in the high byte, command in the low byte. See the divergence
    // note above: this is not the full 32-bit frame.
    int value;

    int address;
    int command;

    // NEC or UNKNOWN.
    int decode_type;

    // Bit count of the frame, 32 for NEC.
    int bits;
};

// --------------------------------------------------------------- IRrecv ----

class IRrecv {
public:
    IRrecv(int pin) {
        pin_ = pin;
        value_ = 0;
    }

    // Configure the pin. Call once, from setup().
    void enableIRIn() { ir_begin(pin_); }

    // Wait for and decode one frame. Returns true on success. The decoded
    // bytes are then readable from this object, or from a decode_results
    // record via decode_into().
    bool decode() {
        if (ir_decode() == 0) return false;
        value_ = ((ir_read_address() & 255) << 8) | (ir_read_command() & 255);
        return true;
    }

    // As decode(), but also fills in a caller-supplied results record — the
    // shape the published API takes by pointer.
    bool decode_into(decode_results *results) {
        if (!decode()) {
            results->decode_type = UNKNOWN;
            results->bits = 0;
            return false;
        }
        results->value = value_;
        results->address = getAddress();
        results->command = getCommand();
        results->decode_type = NEC;
        results->bits = NEC_BITS;
        return true;
    }

    // True once decode() has succeeded and resume() has not yet been called.
    bool available() { return ir_available() != 0; }

    // Release the decoded frame so the next one can be reported.
    void resume() { ir_resume(); }

    // The most recently decoded frame: address in the high byte, command in
    // the low byte.
    int getValue() { return value_; }

    int getAddress() { return (value_ >> 8) & 255; }
    int getCommand() { return value_ & 255; }

private:
    int pin_;
    int value_;
};

#endif // ARDIO_IRREMOTE_H
