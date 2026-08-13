#pragma once
#include "ardio/avr/device.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The emulated machine: guest state, the peripherals hanging off it, and the
// parts wired to its pins.
//
// The execution core translates AVR machine code to native code a block at a
// time rather than decoding instruction by instruction. That is the whole
// design premise, so the state layout here is chosen for a compiler's benefit,
// not an interpreter's: everything the generated code touches lives in one
// flat, fixed-offset struct so a translated block can reach any of it with a
// constant displacement off a single base register.
namespace ardio::emu {

// Guest CPU and memory, in one contiguous block.
//
// Field order is deliberate and load-bearing: r[] first so the register file
// sits at offset 0, then the flags the arithmetic touches constantly, then the
// colder fields. Translated code hardcodes these offsets, so REORDERING THIS
// STRUCT INVALIDATES EVERY CACHED BLOCK. If you change it, bump kStateLayout.
struct State {
    uint8_t r[32] = {};

    // Flags are held as separate bytes rather than packed into SREG, because
    // the host's own flags can be spilled into them with one setcc each. SREG
    // is only materialised when the guest actually reads it, which is rare.
    uint8_t flag_c = 0, flag_z = 0, flag_n = 0, flag_v = 0;
    uint8_t flag_s = 0, flag_h = 0, flag_t = 0, flag_i = 0;

    uint16_t pc = 0;    // word address of the next instruction
    uint16_t sp = 0;

    // Cycles consumed since the machine started. Peripherals are advanced
    // against this, so it is the emulator's clock rather than a statistic.
    uint64_t cycles = 0;

    // The cycle count at which the core must stop and let peripherals run.
    // Translated blocks compare against this on exit, which is how a timer
    // interrupt gets to fire without checking anything per instruction.
    uint64_t deadline = 0;

    uint8_t* sram = nullptr;   // ram_size bytes, at data-space ram_start
    uint8_t* flash = nullptr;  // flash_size bytes
};

// Bumped whenever State's layout or the translator's calling convention
// changes, so a stale cached block can never be executed against a struct it
// was not compiled for.
constexpr uint32_t kStateLayout = 1;

// A memory-mapped peripheral. Anything in data space that is not plain SRAM
// goes through one of these: ports, timers, the USART, the ADC.
//
// Translated code cannot inline these accesses, because a read can have side
// effects and a write can start a conversion. So an access to an address a
// peripheral claims compiles to a call back out here. That is the slow path,
// and it is why claim ranges should be tight.
class Peripheral {
public:
    virtual ~Peripheral() = default;

    // Data-space addresses this peripheral answers for, inclusive.
    virtual bool claims(uint16_t addr) const = 0;

    // These carry no cycle count, so a peripheral has to date an access from
    // the last cycle it was advanced to. That puts an obligation on the core:
    // ADVANCE THIS PERIPHERAL BEFORE EVERY read() OR write() TO IT. Skipping
    // it mistimes whatever the access starts -- a USART frame finishes at the
    // wrong cycle, an ADC conversion completes early -- and nothing here can
    // detect the omission, because a stale peripheral cannot tell that it is
    // stale. The symptom is a sketch whose timing is subtly wrong rather than
    // an error, which is the worst way to find out.
    virtual uint8_t read(uint16_t addr) = 0;
    virtual void write(uint16_t addr, uint8_t value) = 0;

    // Advance to `cycles`. Called when the core reaches a deadline, not per
    // instruction, so this must be written to catch up over an arbitrary span
    // rather than assuming a single tick.
    virtual void advance(uint64_t cycles) = 0;

    // The next cycle count at which this peripheral needs to do something, or
    // UINT64_MAX if it is idle. The core takes the minimum across peripherals
    // to set State::deadline, so a machine doing nothing costs nothing.
    virtual uint64_t next_event() const { return UINT64_MAX; }

    // Vector number to fire, or 0 for none.
    //
    // The core must check this after every advance() AND whenever the guest
    // sets the global interrupt enable, not only when a deadline arrives.
    // Some interrupt sources are level-asserted rather than edge-triggered --
    // UDRE with UDRIE set is the standard example, since the transmit register
    // is empty and stays empty -- so there is no future event for next_event()
    // to schedule and a core that waits for a deadline would sleep through it.
    //
    // A peripheral must NOT work around this by returning a past cycle from
    // next_event() to force a wakeup. That livelocks the core whenever such a
    // flag is asserted while interrupts are globally disabled, which is an
    // ordinary critical section rather than an error.
    virtual uint8_t pending_interrupt() const { return 0; }
    virtual void acknowledge_interrupt() {}
};

// The electrical state of one pin, as seen by whatever is wired to it.
enum class PinState { Low, High, Floating };

// A part on the virtual breadboard: an LED, a button, a servo, a display.
//
// Parts do not see registers. They see pins changing, which is what a real
// component sees, and it keeps a part's implementation independent of which
// board it is plugged into.
class Part {
public:
    virtual ~Part() = default;

    virtual std::string kind() const = 0;   // "led", "button", "servo"

    // A pin this part is wired to changed. `cycles` is when, so a part that
    // cares about timing (a servo measuring pulse width, an IR receiver
    // decoding a bit stream) can measure intervals rather than guess.
    virtual void pin_changed(int pin, PinState state, uint64_t cycles) = 0;

    // What this part is driving onto a pin it can drive, for parts that are
    // inputs to the sketch rather than outputs from it. Floating means the
    // part is not driving that pin at all.
    virtual PinState drive(int pin) const { (void)pin; return PinState::Floating; }

    // A short human-readable line for the CLI, e.g. "led d13: on (blinked 4x)".
    virtual std::string describe() const = 0;
};

struct Wire {
    int pin = 0;
    Part* part = nullptr;
};

// How the core reported back when it stopped.
enum class StopReason {
    Deadline,       // hit State::deadline; peripherals need to run
    Halted,         // executed an infinite self-loop with interrupts off
    IllegalOpcode,  // decoded something that is not an instruction
    Breakpoint,
    Error,
};

struct RunResult {
    StopReason reason = StopReason::Deadline;
    std::string error;   // set when reason is IllegalOpcode or Error
    uint16_t pc = 0;     // where it stopped
};

// The translating execution core.
//
// Implementations translate a basic block starting at a word address into
// native code, cache it, and jump to it. `run` executes until the deadline,
// a halt, or a fault.
class Core {
public:
    virtual ~Core() = default;
    virtual RunResult run(State& state) = 0;

    // Drop cached translations covering this flash range. AVR code can rewrite
    // its own flash with spm, and a bootloader does exactly that, so a cache
    // that never invalidates would run the old code after a self-update.
    virtual void invalidate(uint32_t byte_start, uint32_t byte_end) = 0;

    virtual void set_breakpoint(uint16_t word_addr) = 0;
    virtual void clear_breakpoint(uint16_t word_addr) = 0;

    // Human-readable note on how this core executes, for `ardio emulate
    // --explain`. The point is that a user can tell whether they got the
    // native translator or the portable fallback, since the difference is
    // several orders of magnitude in speed.
    virtual std::string description() const = 0;
};

// Builds the best core available for the host. Returns the portable fallback
// when no native translator exists for this architecture, so emulation always
// works somewhere -- just slower.
std::unique_ptr<Core> make_core(const avr::AvrDevice& device);

// Builds only the portable fallback. Tests use this as the oracle the native
// translator is checked against: the two must agree on final state for every
// program, which is the property that keeps a code generator honest.
std::unique_ptr<Core> make_reference_core(const avr::AvrDevice& device);

} // namespace ardio::emu
