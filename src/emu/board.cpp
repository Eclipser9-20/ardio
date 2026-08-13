// The virtual board: a core, its peripherals, its memory and the parts on its
// pins, joined into something that runs a sketch.
//
// Everything below exists to honour three timing rules that machine.h and
// peripherals.h state but neither can enforce, because both are written from
// the point of view of a component that cannot see the clock. They are restated
// here as the rules this file is responsible for, together with how each one is
// actually kept, since "the core must do X" is only true if some code does X.
//
//   1. A peripheral is advanced before every read or write to it.
//
//      An access carries no cycle count, so a peripheral dates it from the last
//      cycle it was advanced to, and a stale peripheral cannot tell that it is
//      stale. The core is the thing performing the access -- but Core::attach
//      takes a Peripheral, and the reference interpreter routes an access
//      straight through to it. So the advance is interposed: every peripheral
//      is attached to the core through TimedPeripheral below, which advances
//      the real one to State::cycles and only then forwards the access. That
//      puts the obligation in one place instead of once per core, which also
//      means a native translator inherits it for free rather than having to
//      re-implement it correctly.
//
//      An access is dated at the cycle the instruction performing it STARTED,
//      not the cycle it retires, because the core charges an instruction's
//      cycles after it has executed it. That is at most a couple of cycles
//      early on a frame that lasts thousands, and it is early rather than late,
//      which is the direction the peripherals themselves round in.
//
//   2. A pending interrupt is checked after every advance AND whenever the
//      guest sets the global interrupt enable.
//
//      A level-asserted source -- UDRE with UDRIE set is the standard one --
//      has no future event for next_event() to schedule, so a deadline-only
//      check would sleep through it forever. Two things together cover this.
//      The core polls for a pending interrupt before every instruction, so it
//      cannot miss one that a sei just made deliverable; and the run loop below
//      advances every peripheral and then re-enters the core at every deadline,
//      so a flag that only appears as a result of time passing is polled as
//      soon as the peripheral that raises it has been told the time.
//
//   3. State::deadline is the earliest next_event() across the peripherals.
//
//      Not a fixed slice. A machine with nothing scheduled runs to the end of
//      the requested span in one go and pays nothing for the peripherals it is
//      not using, and a machine with a timer running stops exactly when the
//      timer has something to do.
//
// The order inside one turn of the run loop is: advance every peripheral to the
// current cycle, settle the pins, set the deadline, run. It is worth saying why
// that order and not another, because getting it wrong does not fail -- it
// produces a sketch that behaves subtly incorrectly.
//
//   * Advancing comes first because the core has just stopped at a deadline,
//     which is a moment some peripheral asked for. Until it is advanced it has
//     not noticed its own event: the timer has not set TOV0, the USART has not
//     finished its frame. Running the core before advancing would execute the
//     instructions of that cycle against hardware that is still in the previous
//     state, which is precisely the interrupt that fires one wakeup late.
//
//   * Settling the pins comes after advancing and before running, so that a
//     part which changed what it drives while the core was busy is on the pin
//     before the guest's next instruction can read it.
//
//   * The deadline is computed after advancing, never before, because advancing
//     is what moves a peripheral's next event forward. Computing it first would
//     hand back the event that has just been consumed and the core would return
//     immediately, forever.
//
//   * The core runs last, and while it runs it reaches peripherals only through
//     TimedPeripheral, so rule 1 continues to hold for accesses that happen
//     inside the span rather than at its edges.

#include "ardio/emu/board.h"

#include "ardio/avr/device.h"
#include "ardio/emu/parts.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace ardio::emu {
namespace {

constexpr uint64_t kNever = UINT64_MAX;

// What TimedPeripheral needs from the machine. It exists so a peripheral proxy
// can be written before the machine that owns it, and so the proxy can reach
// exactly two things -- the clock and the pin settling -- rather than the whole
// machine.
class MachineHooks {
public:
    virtual ~MachineHooks() = default;
    virtual uint64_t now() const = 0;
    virtual void settle_pins() = 0;
};

// A peripheral, wrapped so that it is always current when it is touched.
//
// This is the whole of rule 1. Everything else forwards unchanged, so the
// wrapper is invisible to the core and to the peripheral alike.
class TimedPeripheral final : public Peripheral {
public:
    TimedPeripheral(Peripheral* inner, GpioPort* port, MachineHooks* hooks)
        : inner_(inner), port_(port), hooks_(hooks) {}

    bool claims(uint16_t addr) const override { return inner_->claims(addr); }

    uint8_t read(uint16_t addr) override {
        // A read of a port has to see what the parts are driving right now, or
        // a button pressed since the last deadline reads as unpressed. Settling
        // first is also what makes a part's drive the thing the guest reads
        // back, since GpioPort resolves PINx from its external levels.
        if (port_) hooks_->settle_pins();
        inner_->advance(hooks_->now());
        return inner_->read(addr);
    }

    void write(uint16_t addr, uint8_t value) override {
        inner_->advance(hooks_->now());
        inner_->write(addr, value);
        // A write to a port may have changed a pin the sketch drives. Settling
        // here rather than at the next deadline is what lets a part be told the
        // cycle the change happened on, which a part measuring a pulse width
        // depends on entirely.
        if (port_) hooks_->settle_pins();
    }

    void advance(uint64_t cycles) override { inner_->advance(cycles); }
    uint64_t next_event() const override { return inner_->next_event(); }
    uint8_t pending_interrupt() const override { return inner_->pending_interrupt(); }
    void acknowledge_interrupt() override { inner_->acknowledge_interrupt(); }

private:
    Peripheral* inner_ = nullptr;
    GpioPort* port_ = nullptr;   // null unless `inner` is that port
    MachineHooks* hooks_ = nullptr;
};

// Where an Arduino pin number lands.
struct PinLocation {
    GpioPort* port = nullptr;
    uint8_t bit = 0;
};

class BoardMachine final : public Machine, private MachineHooks {
public:
    BoardMachine(const Board& board, const avr::AvrDevice& device)
        : board_(board), device_(device) {
        flash_.assign(device_.flash_size, 0xFF);
        sram_.assign(device_.ram_size, 0);
        pin_level_.assign(device_.pins.size(), PinState::Floating);
        for (double& v : analog_volts_) v = 0.0;
        build_hardware();
    }

    bool load(const std::vector<uint8_t>& flash, std::string& error) override;
    void reset() override;
    StepResult run_for(uint64_t cycles) override;
    StepResult run_ms(uint64_t milliseconds) override;
    void wire(int pin, Part* part) override;
    std::string take_serial_output() override;
    void feed_serial(const std::string& bytes) override;
    void set_analog_volts(int analog_input, double volts) override;
    const State& state() const override { return state_; }
    uint64_t cycles() const override { return state_.cycles; }
    std::string description() const override;

private:
    // ---- MachineHooks -------------------------------------------------
    uint64_t now() const override { return state_.cycles; }
    void settle_pins() override { settle(); }

    // ---- construction -------------------------------------------------
    void build_hardware();

    // ---- the run loop -------------------------------------------------
    void advance_all();
    uint64_t next_deadline(uint64_t target) const;
    StepResult finish(RunOutcome outcome, const std::string& error);

    // ---- pins ---------------------------------------------------------
    PinLocation locate(int pin) const;
    PinState resolve(const PinLocation& loc) const;
    void settle();

    Board board_;
    avr::AvrDevice device_;

    std::vector<uint8_t> flash_;
    std::vector<uint8_t> sram_;
    State state_;

    PeripheralSet peripherals_;
    std::vector<std::unique_ptr<TimedPeripheral>> proxies_;
    std::unique_ptr<Core> core_;

    std::vector<Wire> wires_;
    std::vector<int> wired_pins_;          // the distinct pins in `wires_`
    std::vector<PinState> pin_level_;      // last level reported to the parts

    // Analog inputs are held here as well as in the ADC, because a reset
    // rebuilds the ADC and the voltage on a potentiometer does not go away
    // because the board was reset.
    double analog_volts_[16] = {};

    // Once a sketch has halted it stays halted: nothing left in the machine can
    // change, so a second run must report that immediately rather than charging
    // cycles for a loop that will never end.
    bool stopped_ = false;
    RunOutcome stopped_outcome_ = RunOutcome::Halted;
    std::string stopped_error_;
};

// ------------------------------------------------------- construction ---

void BoardMachine::build_hardware() {
    // The proxies must be destroyed before the peripherals they point at, and
    // the core before the proxies it was attached to, so everything is torn
    // down here in that order rather than left to member declaration order.
    core_.reset();
    proxies_.clear();
    peripherals_ = make_standard_peripherals(device_);

    core_ = make_core(device_);
    for (std::unique_ptr<Peripheral>& p : peripherals_.owned) {
        GpioPort* port = nullptr;
        for (GpioPort* candidate : peripherals_.ports)
            if (candidate == p.get()) port = candidate;
        MachineHooks* hooks = this;
        proxies_.push_back(std::make_unique<TimedPeripheral>(p.get(), port, hooks));
        core_->attach(proxies_.back().get());
    }

    if (peripherals_.adc) {
        // A board's supply is not in the board description, so 5 V is assumed.
        // It is the reference analogRead resolves against, so a 3.3 V board
        // emulated as 5 V would return readings that are consistently low --
        // worth knowing about before trusting a value out of this.
        peripherals_.adc->set_supply_voltage(5.0);
        peripherals_.adc->set_aref_voltage(5.0);
        for (uint8_t c = 0; c < 16; ++c)
            peripherals_.adc->set_channel_voltage(c, analog_volts_[c]);
    }
}

// -------------------------------------------------------------- load ---

bool BoardMachine::load(const std::vector<uint8_t>& flash, std::string& error) {
    if (flash.size() > device_.flash_size) {
        // Loading the part that fits would emulate a truncated sketch and
        // report behaviour the real part could never produce, which is worse
        // than not running at all.
        error = "the program is " + std::to_string(flash.size()) + " bytes but " +
                board_.name + " has " + std::to_string(device_.flash_size) +
                " bytes of flash";
        return false;
    }

    flash_.assign(device_.flash_size, 0xFF);   // erased flash, as on real silicon
    std::copy(flash.begin(), flash.end(), flash_.begin());
    reset();
    error.clear();
    return true;
}

void BoardMachine::reset() {
    state_ = State{};
    std::fill(sram_.begin(), sram_.end(), uint8_t(0));
    state_.flash = flash_.data();
    state_.sram = sram_.data();
    // The runtime's own reset code sets the stack pointer before it does
    // anything else. Starting it at RAMEND anyway means a hand-written program
    // that skips that step still has a usable stack rather than pushing over
    // the register file.
    state_.sp = device_.ramend;

    // The peripherals hold their own idea of the current cycle, and nothing on
    // the Peripheral interface resets it. Rebuilding them is the only way to
    // put the whole machine back at cycle zero -- and since the core holds
    // pointers to them, with no way to detach, the core is rebuilt with them.
    build_hardware();

    std::fill(pin_level_.begin(), pin_level_.end(), PinState::Floating);
    stopped_ = false;
    stopped_error_.clear();
    settle();
}

// ---------------------------------------------------------- run loop ---

void BoardMachine::advance_all() {
    for (std::unique_ptr<Peripheral>& p : peripherals_.owned) p->advance(state_.cycles);
}

uint64_t BoardMachine::next_deadline(uint64_t target) const {
    uint64_t next = kNever;
    for (const std::unique_ptr<Peripheral>& p : peripherals_.owned)
        next = std::min(next, p->next_event());

    uint64_t deadline = std::min(target, next);
    // A peripheral may name a cycle that has already passed -- it is advanced
    // to the current cycle at the top of every turn, so this can only be a
    // rounding artefact, but a deadline at or behind the clock would make the
    // core return without executing anything and the loop would not terminate.
    // One cycle of progress is enough to guarantee it does.
    if (deadline <= state_.cycles) deadline = state_.cycles + 1;
    return std::min(deadline, target);
}

StepResult BoardMachine::finish(RunOutcome outcome, const std::string& error) {
    StepResult out;
    out.outcome = outcome;
    out.cycles = state_.cycles;
    out.error = error;
    return out;
}

StepResult BoardMachine::run_for(uint64_t cycles) {
    if (stopped_) return finish(stopped_outcome_, stopped_error_);

    const uint64_t target = state_.cycles + cycles;

    for (;;) {
        advance_all();
        settle();

        if (state_.cycles >= target) return finish(RunOutcome::ReachedTime, {});

        state_.deadline = next_deadline(target);
        RunResult result = core_->run(state_);

        switch (result.reason) {
        case StopReason::Deadline:
            // The ordinary case: something is due, or the span has run out.
            // Either way the next turn of the loop advances the peripherals and
            // works out which it was.
            break;

        case StopReason::Halted:
            // A self-loop with interrupts off. Advance and settle once more so
            // that a byte still on the wire lands and a part sees the final pin
            // level, then latch it: nothing can change after this.
            advance_all();
            settle();
            stopped_ = true;
            stopped_outcome_ = RunOutcome::Halted;
            return finish(RunOutcome::Halted, {});

        case StopReason::IllegalOpcode:
        case StopReason::Error:
            stopped_ = true;
            stopped_outcome_ = RunOutcome::Fault;
            stopped_error_ = result.error;
            return finish(RunOutcome::Fault, result.error);

        case StopReason::Breakpoint:
            // Nothing sets a breakpoint on this machine, so reaching one means
            // the sketch executed a `break` instruction -- a debugger trap with
            // no debugger attached. Reporting it as a fault names what happened
            // instead of quietly presenting it as a normal end of run.
            stopped_ = true;
            stopped_outcome_ = RunOutcome::Fault;
            stopped_error_ = "the program executed a break instruction at word "
                             "address " + std::to_string(result.pc) +
                             ", which traps to a debugger this machine does not have";
            return finish(RunOutcome::Fault, stopped_error_);
        }
    }
}

StepResult BoardMachine::run_ms(uint64_t milliseconds) {
    // Simulated time is the honest unit for a caller, and it is the board's
    // clock rather than the part's that decides how many cycles a millisecond
    // is: the same silicon runs at 16 MHz on an Uno and 8 MHz on a 3.3 V Pro
    // Mini, and a sketch's delays differ accordingly.
    return run_for(uint64_t(board_.f_cpu) * milliseconds / 1000);
}

// -------------------------------------------------------------- pins ---

PinLocation BoardMachine::locate(int pin) const {
    PinLocation loc;
    if (pin < 0 || size_t(pin) >= device_.pins.size()) return loc;
    const avr::PinMapping& m = device_.pins[size_t(pin)];
    if (m.pin_reg == 0) return loc;   // a number this part does not bring out
    loc.port = peripherals_.port_for(m.pin_reg);
    loc.bit = m.bit;
    return loc;
}

PinState BoardMachine::resolve(const PinLocation& loc) const {
    if (!loc.port) return PinState::Floating;

    // The order is the one GpioPort itself resolves PINx in, and it must be the
    // same order or a part would be told a level the guest does not read. A
    // part driving the line wins over the MCU's output driver: shorting an
    // output to a rail is an electrical fault, and reporting where the pin
    // actually ends up is more useful than pretending the conflict cannot
    // happen.
    PinState external = loc.port->external(loc.bit);
    if (external != PinState::Floating) return external;

    PinState driven = loc.port->drive(loc.bit);
    if (driven != PinState::Floating) return driven;

    // An input with nothing on it. The pull-up only decides the level once
    // nothing else does, which is the whole reason PORTx cannot be modelled as
    // the pin level.
    return loc.port->pullup(loc.bit) ? PinState::High : PinState::Floating;
}

void BoardMachine::settle() {
    if (wires_.empty()) return;

    // Parts are dated exactly the way peripherals are: drive() carries no cycle
    // count either, so a part reports what it is driving as of its last
    // advance. Advancing here rather than only at a deadline is what makes a
    // scripted button press land on the cycle it was scripted for instead of
    // whenever the sketch next happened to poll something -- and settle() is
    // called both from the run loop, after the peripherals are advanced, and
    // from every port access, which is where drive() is about to be read.
    for (size_t i = 0; i < wires_.size(); ++i) {
        Part* part = wires_[i].part;
        if (!part) continue;
        // A part wired to several pins appears in `wires_` once per pin.
        // Advancing it once per pin would be harmless for a part that tracks a
        // cycle count and wrong for one that accumulates, so it is advanced
        // once.
        bool seen = false;
        for (size_t j = 0; j < i && !seen; ++j) seen = wires_[j].part == part;
        if (!seen) part->advance(state_.cycles);
    }

    // First publish what every part is driving, so that the ports resolve
    // against the current outside world. This has to be a separate pass:
    // deciding a pin's level before all the parts on it have been asked would
    // make the answer depend on the order the parts were wired in.
    for (int pin : wired_pins_) {
        PinLocation loc = locate(pin);
        if (!loc.port) continue;
        PinState external = PinState::Floating;
        for (const Wire& w : wires_) {
            if (w.pin != pin || !w.part) continue;
            PinState d = w.part->drive(pin);
            if (d != PinState::Floating) { external = d; break; }
        }
        loc.port->set_external(loc.bit, external);
    }

    // Then tell the parts about pins whose level changed. Only changes are
    // reported: pin_changed is an edge a part counts or times, and re-reporting
    // an unchanged level at every deadline would make a button look like it was
    // being pressed thousands of times a second.
    for (int pin : wired_pins_) {
        PinLocation loc = locate(pin);
        if (!loc.port) continue;
        PinState level = resolve(loc);
        if (level == pin_level_[size_t(pin)]) continue;
        pin_level_[size_t(pin)] = level;
        for (const Wire& w : wires_)
            if (w.pin == pin && w.part) w.part->pin_changed(pin, level, state_.cycles);
    }
}

void BoardMachine::wire(int pin, Part* part) {
    if (!part) return;

    PinState before = PinState::Floating;
    bool have_pin = pin >= 0 && size_t(pin) < pin_level_.size();
    if (have_pin) before = pin_level_[size_t(pin)];

    // A part converts cycles to microseconds to do anything with time, and it
    // cannot know the rate from a pin number. Setting it here means a part that
    // has been wired to a machine can never be left without one: this is the
    // only moment at which a part and a board are both in hand. A part from
    // outside this library says so by returning false, and there is nothing to
    // do about that -- it has its own arrangements.
    set_part_clock(part, uint32_t(board_.f_cpu));

    wires_.push_back(Wire{pin, part});
    if (std::find(wired_pins_.begin(), wired_pins_.end(), pin) == wired_pins_.end())
        wired_pins_.push_back(pin);

    // Wiring a part to a pin the board does not have is recorded and inert
    // rather than refused: Machine::wire has no way to report an error, and a
    // part that is never told about a pin cannot act on one.
    settle();

    // A part has to know the level the pin is ALREADY at -- an LED wired to a
    // pin the sketch drove high before wiring is on, and a part that only ever
    // hears about changes would have it off until the next one. If settle()
    // above already reported a change, that was the notification and this would
    // be a duplicate, so it is sent only when the level did not move.
    if (have_pin && pin_level_[size_t(pin)] == before) {
        PinLocation loc = locate(pin);
        if (loc.port) part->pin_changed(pin, before, state_.cycles);
    }
}

// ------------------------------------------------------------ serial ---

std::string BoardMachine::take_serial_output() {
    if (!peripherals_.usart0) return {};
    // A frame that finished while the core was running is only recorded when
    // the USART is told the time, so this advances before reading rather than
    // reporting a byte one call late.
    peripherals_.usart0->advance(state_.cycles);
    std::string out = peripherals_.usart0->output();
    peripherals_.usart0->clear_output();
    return out;
}

void BoardMachine::feed_serial(const std::string& bytes) {
    if (!peripherals_.usart0) return;
    // The receiver times the incoming frame from the cycle it starts, which is
    // now -- so the USART has to be at `now` before it is handed the bytes, or
    // they would arrive as early as it is behind.
    peripherals_.usart0->advance(state_.cycles);
    peripherals_.usart0->feed(bytes);
}

// ------------------------------------------------------ analog input ---

void BoardMachine::set_analog_volts(int analog_input, double volts) {
    // A0 answers to two numbers on a board numbered like the Uno: 0 and 14. A
    // caller holding a pin number should not have to know which one this wants,
    // and the two ranges cannot overlap -- analog_pin_base is past the end of
    // the analog numbering by construction -- so both are accepted.
    int index = analog_input;
    if (device_.analog_pin_base != 0 && index >= int(device_.analog_pin_base))
        index -= int(device_.analog_pin_base);

    if (index < 0 || size_t(index) >= device_.analog.size()) return;
    const avr::AnalogMapping& m = device_.analog[size_t(index)];
    if (!m.exists || m.channel >= 16) return;

    analog_volts_[m.channel] = volts;
    if (peripherals_.adc) peripherals_.adc->set_channel_voltage(m.channel, volts);
}

// ------------------------------------------------------- description ---

std::string BoardMachine::description() const {
    std::string out = board_.name + ", " + device_.name + " at " +
                      std::to_string(board_.f_cpu / 1000000) + " MHz";

    out += ", with ";
    std::string parts;
    for (const GpioPort* port : peripherals_.ports) {
        parts += parts.empty() ? "port " : "/";
        parts += port->label();
    }
    if (peripherals_.timer0) parts += (parts.empty() ? "" : ", ") + std::string("timer 0");
    if (peripherals_.usart0) parts += (parts.empty() ? "" : ", ") + std::string("USART0");
    if (peripherals_.adc) parts += (parts.empty() ? "" : ", ") + std::string("the ADC");
    out += parts.empty() ? "no peripherals" : parts;

    // The core describes itself rather than being described from here, so this
    // cannot claim a native translator that was not actually built.
    out += ". Executing with the " + core_->description() + ".";
    return out;
}

} // namespace

std::unique_ptr<Machine> Machine::create(const Board& board, std::string& error) {
    const avr::AvrDevice* device = avr::find_device(board.mcu);
    if (!device) {
        // Emulating some other part would be worse than refusing: the register
        // addresses, the pin numbering and the vector table would all be
        // somebody else's, and the run would look plausible throughout.
        error = "ardio has no device description for " + board.mcu + ", which " +
                board.name + " uses, so it cannot be emulated. Add one to the "
                "table in src/avr/device.cpp.";
        return nullptr;
    }
    if (device->far_flash) {
        // State::pc and the Z pointer are 16 bits, which stops covering flash
        // past 64 KB. Running anyway would fetch from the wrong half of the
        // image as soon as a sketch grew past that line.
        error = board.name + " uses the " + board.mcu + ", which has " +
                std::to_string(device->flash_size / 1024) +
                " KB of flash. The emulator addresses flash with 16 bits, so it "
                "cannot run this part.";
        return nullptr;
    }
    if (board.f_cpu <= 0) {
        // Without a clock rate there is no relation between cycles and time, so
        // run_ms would have to invent one.
        error = board.name + " has no clock rate in the board description, so "
                "simulated time has no meaning for it.";
        return nullptr;
    }

    error.clear();
    return std::make_unique<BoardMachine>(board, *device);
}

} // namespace ardio::emu
