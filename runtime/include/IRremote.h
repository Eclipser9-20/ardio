// IRremote.h — ardio's own NEC infrared receive class, API-compatible with the
// commonly used IRrecv sketch interface.
//
// The decoding lives in ardio's assembly runtime (runtime/ir.S), which samples
// the receiver module's output with cycle-counted timing loops and rebuilds a
// NEC frame from the published protocol timings. Nothing here is derived from
// any existing IR library; this header only declares the shape of the API.
//
// Wiring: a TSOP-style receiver module's OUT pin goes to any digital pin. Its
// output is active low — idle high, pulled low by a burst of carrier — and the
// runtime enables the internal pull-up on the pin.
//
// Written inside the C++ subset ardio's own compiler accepts; see Arduino.h for
// the full list. The ones that bite here:
//
//   * No `extern "C"` and no typedefs, so the runtime entry points are plain
//     declarations using built-in types.
//   * No overloads. The published API overloads decode(); here the no-argument
//     form is decode() and the form that fills a results record is
//     decode_into().
//   * No 32-bit expressions. The published API hands back the whole 32-bit NEC
//     frame in decode_results::value; the code generator evaluates 16 bits
//     wide, so value carries the meaningful half — address in the high byte,
//     command in the low byte — and the two bytes are also available on their
//     own as address and command. Sketches that compare against a full 32-bit
//     remote code should compare command instead.
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
    decode_results();

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
    IRrecv(int pin);

    // Configure the pin. Call once, from setup().
    void enableIRIn();

    // Wait for and decode one frame. Returns true on success. The decoded
    // bytes are then readable from this object, or from a decode_results
    // record via decode_into().
    bool decode();

    // As decode(), but also fills in a caller-supplied results record — the
    // shape the published API takes by pointer.
    bool decode_into(decode_results *results);

    // True once decode() has succeeded and resume() has not yet been called.
    bool available();

    // Release the decoded frame so the next one can be reported.
    void resume();

    // The most recently decoded frame: address in the high byte, command in
    // the low byte.
    int getValue();

    int getAddress();
    int getCommand();

private:
    int pin_;
    int value_;
};

#endif // ARDIO_IRREMOTE_H
