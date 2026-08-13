#pragma once
#include "ardio/board.h"
#include "ardio/emu/machine.h"
#include "ardio/emu/peripherals.h"
#include <memory>
#include <string>
#include <vector>

// A whole virtual board: a core, the peripherals wired into it, the memory it
// runs out of, and the parts plugged into its pins.
//
// This is the piece that makes the emulator a board rather than a CPU. It owns
// the run loop, which is where the three timing rules the other headers state
// are actually honoured -- advance peripherals before an access, check for
// interrupts when the guest enables them, and take the deadline from the
// earliest thing any peripheral is waiting for.
namespace ardio::emu {

// How a run ended, in terms a caller cares about rather than core internals.
// There is deliberately no "a part asked to stop" outcome. One was specified
// and then removed, because Part has no way to ask: nothing could ever produce
// it, and an outcome a caller must handle but can never observe is worse than
// no outcome at all -- it reads as a supported case and is untestable. Add it
// back together with the mechanism on Part, not before.
enum class RunOutcome {
    ReachedTime,   // ran out the requested span, still running happily
    Halted,        // the sketch stopped: a self-loop with interrupts off
    Fault,         // illegal instruction or a core error
};

struct StepResult {
    RunOutcome outcome = RunOutcome::ReachedTime;
    uint64_t cycles = 0;   // total elapsed since reset
    std::string error;     // set when outcome is Fault
};

class Machine {
public:
    // `board` supplies f_cpu and the part name; the device description follows
    // from it. Fails if ardio has no device for that board, rather than
    // silently emulating something else.
    static std::unique_ptr<Machine> create(const Board& board, std::string& error);
    virtual ~Machine() = default;

    // Loads a program image at flash byte 0 and resets. Rejects an image that
    // does not fit, since the alternative is emulating a truncated sketch and
    // reporting behaviour the real part would never produce.
    virtual bool load(const std::vector<uint8_t>& flash, std::string& error) = 0;
    virtual void reset() = 0;

    // Runs until `cycles` more have elapsed, or something stops it earlier.
    virtual StepResult run_for(uint64_t cycles) = 0;

    // Runs for a span of simulated time. This is the honest unit for a caller:
    // "one second of sketch time" means the same thing whatever the board's
    // clock is, and whatever speed the host manages.
    virtual StepResult run_ms(uint64_t milliseconds) = 0;

    // Wires a part to a pin. The machine does not take ownership, so a caller
    // can keep querying the part it wired.
    virtual void wire(int pin, Part* part) = 0;

    // Everything the sketch has written to Serial since the last call.
    virtual std::string take_serial_output() = 0;
    virtual void feed_serial(const std::string& bytes) = 0;

    // Analog input, in volts, as a part or a script would set it.
    virtual void set_analog_volts(int analog_input, double volts) = 0;

    virtual const State& state() const = 0;
    virtual uint64_t cycles() const = 0;

    // How this machine executes, for `ardio emulate --explain`: which core it
    // got and therefore roughly how fast it will be.
    virtual std::string description() const = 0;
};

} // namespace ardio::emu
