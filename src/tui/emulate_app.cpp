// The application shell behind `ardio emulate --tui`.
//
// The screen exists to show timing, so the timing has to survive being looked
// at. Two rules follow from that and everything below is arranged around them.
//
//   1. The machine is sampled between runs and never during a draw. A
//      peripheral access is dated at the current cycle, so reading a port from
//      inside a draw would advance the very thing being drawn. The loop
//      therefore runs a slice, takes one Snapshot, and then draws that
//      Snapshot as often as it likes -- a resize repaints from the same
//      snapshot rather than sampling a new one.
//
//   2. The speed on the screen is measured, not requested. The emulator is
//      roughly forty times faster than the silicon, so an unthrottled blink
//      sketch is a flicker; the loop asks for real time by default and then
//      reports the wall time it actually took. When the host cannot deliver
//      the requested rate the two numbers separate, and both are shown, which
//      is the only honest thing to do with a pace we asked for and missed.
//
// The pacing, key handling, layout and snapshot construction live in app.h as
// pure functions. There is no terminal in CI, so anything that has to be
// tested has to be reachable without one, and what remains here is the part
// that genuinely cannot be: the libhike session and the loop that calls the
// rest in order.

#include "ardio/tui/app.h"

#include "ardio/avr/device.h"
#include "ardio/emu/parts.h"
#include "ardio/tui/emulate_view.h"
#include "hike/hike.h"
#include "hike/widgets.h"

// libhike resizes nothing on a HIKE_EVENT_RESIZE: it reports the new size and
// leaves the grid alone, because only the caller knows whether its layout can
// survive one. Reallocating the grid is therefore this file's job, and the
// call that does it is not part of hike.h. Reaching into the internal header
// is deliberate and is the only such reach here; libhike is part of this tree,
// so this is a note about where a public hike_resize belongs rather than a
// layering violation across a library boundary.
#include "../../libhike/src/hike_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace ardio::tui {
namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Formats a speed the way the status bar wants it: enough digits to tell 0.05
// from 0.5, and none of the trailing zeroes that make a number look measured
// to more precision than it is.
std::string format_speed(double speed) {
    char buf[32];
    if (speed >= 10.0) std::snprintf(buf, sizeof(buf), "%.0fx", speed);
    else if (speed >= 1.0) std::snprintf(buf, sizeof(buf), "%.3gx", speed);
    else std::snprintf(buf, sizeof(buf), "%.2gx", speed);
    return buf;
}

int count_lines(const std::string& text) {
    if (text.empty()) return 0;
    int lines = 0;
    for (char c : text) if (c == '\n') ++lines;
    // A last line with no newline after it is still a line on the screen.
    if (text.back() != '\n') ++lines;
    return lines;
}

// Restores the terminal however the session ends.
//
// libhike installs handlers for the fatal signals it can, but a return through
// an error path and a thrown exception are both ordinary control flow that no
// signal handler will ever see, and either one leaving raw mode set hands the
// user a shell that does not echo. That makes this a destructor rather than a
// call at the end of the loop.
class Session {
public:
    explicit Session(hike_context* ctx) : ctx_(ctx) {}
    ~Session() { if (ctx_) hike_shutdown(ctx_); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

private:
    hike_context* ctx_ = nullptr;
};

// Everything the loop carries between frames that is not AppState.
struct Running {
    std::string serial;
    double host_seconds = 0.0;       // wall time spent running, never time paused
    double measured_cps = 0.0;       // cycles per host second, for the unthrottled slice
    double sleep_seconds = 0.0;      // what the last slice owes the requested rate
    bool keeping_up = true;
    bool halted = false;
    std::string fault;
};

// Runs one slice and folds what it cost into the measurements.
void advance(emu::Machine& machine, const Board& board, const AppState& state,
             Running& run, uint64_t cycles) {
    if (cycles == 0) return;

    const uint64_t before = machine.cycles();
    const Clock::time_point started = Clock::now();
    emu::StepResult result = machine.run_for(cycles);
    const double spent = seconds_since(started);

    const uint64_t done = machine.cycles() - before;
    run.host_seconds += spent;
    if (spent > 0.0 && done > 0) {
        // Smoothed, because one frame's throughput swings with whatever else
        // the host is doing and a slice size that swings with it would make
        // the frame rate visibly uneven.
        const double sample = double(done) / spent;
        run.measured_cps = run.measured_cps > 0.0
                               ? run.measured_cps * 0.75 + sample * 0.25
                               : sample;
    }

    PaceResult pace = pace_after_run(board.f_cpu, current_speed(state), done, spent);
    run.keeping_up = pace.keeping_up;
    if (pace.sleep_seconds > 0.0) {
        // The wait is not a sleep. It is spent in hike_poll below, so a key
        // pressed during it is answered immediately rather than up to a frame
        // later, which is what makes pause feel instant at 0.05x.
        run.sleep_seconds = pace.sleep_seconds;
    } else {
        run.sleep_seconds = 0.0;
    }

    switch (result.outcome) {
    case emu::RunOutcome::ReachedTime:
        break;
    case emu::RunOutcome::Halted:
        run.halted = true;
        break;
    case emu::RunOutcome::Fault:
        run.fault = result.error;
        break;
    }

    run.serial += machine.take_serial_output();
}

} // namespace

// ------------------------------------------------------------- pacing ----

uint64_t pace_cycles(int f_cpu, double speed, double frame_seconds,
                     double measured_cycles_per_second) {
    if (f_cpu <= 0 || frame_seconds <= 0.0) return 0;

    double cycles;
    if (speed > 0.0) {
        cycles = double(f_cpu) * speed * frame_seconds;
    } else {
        // Unthrottled: there is no requested rate to convert, so the slice is
        // sized from what the host has actually managed. Before anything has
        // been measured the board's own clock is the only number in hand, and
        // a slice of a frame of real time is a safe start -- too small merely
        // costs an extra frame, while a guess that is too large would freeze
        // the keyboard for as long as it took to run.
        const double cps = measured_cycles_per_second > 0.0
                               ? measured_cycles_per_second
                               : double(f_cpu);
        cycles = cps * frame_seconds;
    }

    if (cycles < 1.0) return 1;   // a frame must make progress or nothing moves
    return uint64_t(cycles);
}

PaceResult pace_after_run(int f_cpu, double speed, uint64_t cycles_run,
                          double spent_seconds) {
    PaceResult out;
    // Unthrottled asked for no particular rate, so there is no rate to miss
    // and nothing to hold back for.
    if (f_cpu <= 0 || speed <= 0.0 || cycles_run == 0) return out;

    const double due = double(cycles_run) / (double(f_cpu) * speed);
    out.sleep_seconds = due - spent_seconds;
    if (out.sleep_seconds < 0.0) out.sleep_seconds = 0.0;

    // A little tolerance, because a frame that overran by a fraction of a
    // percent is a scheduling hiccup rather than a host that cannot keep up,
    // and a warning that flickers on and off is a warning nobody reads.
    out.keeping_up = spent_seconds <= due * 1.05;
    return out;
}

// ----------------------------------------------------------- keyboard ----

double current_speed(const AppState& state) {
    int i = state.speed_index;
    if (i < 0) i = 0;
    if (i >= kSpeedCount) i = kSpeedCount - 1;
    return kSpeedLadder[i];
}

bool unthrottled(const AppState& state) { return current_speed(state) <= 0.0; }

int max_serial_scroll(const AppState& state) {
    const int hidden = state.serial_lines - state.serial_rows;
    return hidden > 0 ? hidden : 0;
}

KeyAction apply_key(AppState& state, const hike_key_event& key) {
    const int page = state.serial_rows > 1 ? state.serial_rows - 1 : 1;
    const int limit = max_serial_scroll(state);

    auto scroll_by = [&](int lines) {
        state.serial_scroll += lines;
        if (state.serial_scroll < 0) state.serial_scroll = 0;
        if (state.serial_scroll > limit) state.serial_scroll = limit;
    };

    switch (key.key) {
    case HIKE_KEY_UP:        scroll_by(1);      return KeyAction::None;
    case HIKE_KEY_DOWN:      scroll_by(-1);     return KeyAction::None;
    case HIKE_KEY_PAGE_UP:   scroll_by(page);   return KeyAction::None;
    case HIKE_KEY_PAGE_DOWN: scroll_by(-page);  return KeyAction::None;
    case HIKE_KEY_HOME:      scroll_by(limit);  return KeyAction::None;
    case HIKE_KEY_END:       scroll_by(-limit); return KeyAction::None;
    case HIKE_KEY_ESCAPE:    return KeyAction::Quit;
    case HIKE_KEY_CHAR:      break;
    default:                 return KeyAction::None;
    }

    // Ctrl-C is a quit here rather than a signal: raw mode means the terminal
    // no longer generates one, so a user pressing it and getting nothing would
    // reasonably conclude the program had hung.
    if ((key.mods & HIKE_MOD_CTRL) && (key.ch == 'c' || key.ch == 3))
        return KeyAction::Quit;

    switch (key.ch) {
    case 'q':
        return KeyAction::Quit;

    case ' ':
        state.paused = !state.paused;
        return KeyAction::None;

    case 's':
        // Stepping while the machine is running would be invisible, since the
        // next slice runs millions of cycles over it. A step therefore pauses
        // first, which also means space-then-s-then-space does what it reads
        // like: stop, look at one instruction, carry on.
        state.paused = true;
        return KeyAction::StepInstruction;

    case 'n':
        state.paused = true;
        return KeyAction::StepSlice;

    case 'r':
        return KeyAction::Reset;

    case '+':
    case '=':
        if (state.speed_index + 1 < kSpeedCount) ++state.speed_index;
        return KeyAction::None;

    case '-':
    case '_':
        if (state.speed_index > 0) --state.speed_index;
        return KeyAction::None;

    case '0':
        state.speed_index = kSpeedCount - 1;   // the unthrottled end of the ladder
        return KeyAction::None;

    case '1':
        state.speed_index = kSpeedRealTime;
        return KeyAction::None;

    default:
        return KeyAction::None;
    }
}

const char* key_help_text() {
    return "space pause  s step  n slice  r reset  -/+ speed  1 real time  "
           "0 full  up/down/pgup/pgdn scroll  q quit";
}

// ------------------------------------------------------------- layout ----

AppLayout compute_layout(hike_rect screen) {
    AppLayout out;
    if (screen.w <= 0 || screen.h <= 0) return out;

    // The status line and the key bindings are declared before the body, so
    // when the terminal is too short for everything it is the panes that
    // vanish and the two lines that tell the user where they are and how to
    // get out that survive. That is the order the split serves fixed children
    // in, and it is the reason this is a split rather than arithmetic.
    const hike_size column[] = {hike_fixed(1), hike_fixed(1), hike_weight(1)};
    hike_rect rows[3];
    hike_layout col = hike_column();
    if (hike_layout_split(col, screen, column, 3, rows) != 3) return out;
    out.status = rows[0];
    out.help = rows[1];
    hike_rect body = rows[2];

    if (body.w <= 0 || body.h <= 0) return out;

    // Below this width two columns leave nothing readable in either, so the
    // panes stack instead and the user scrolls the terminal's own height.
    constexpr int kTwoColumnMinWidth = 60;
    if (body.w < kTwoColumnMinWidth) {
        const hike_size stack[] = {hike_weight(3), hike_weight(2), hike_weight(2),
                                   hike_weight(3)};
        hike_rect panes[4];
        if (hike_layout_split(hike_column(), body, stack, 4, panes) != 4) return out;
        out.board = panes[0];
        out.parts = panes[1];
        out.cpu = panes[2];
        out.serial = panes[3];
        return out;
    }

    const hike_size halves[] = {hike_weight(1), hike_weight(1)};
    hike_rect sides[2];
    if (hike_layout_split(hike_row(), body, halves, 2, sides) != 2) return out;

    const hike_size left_rows[] = {hike_weight(3), hike_weight(2)};
    hike_rect left[2];
    if (hike_layout_split(hike_column(), sides[0], left_rows, 2, left) != 2) return out;
    out.board = left[0];
    out.cpu = left[1];

    const hike_size right_rows[] = {hike_weight(2), hike_weight(3)};
    hike_rect right[2];
    if (hike_layout_split(hike_column(), sides[1], right_rows, 2, right) != 2) return out;
    out.parts = right[0];
    out.serial = right[1];
    return out;
}

// ----------------------------------------------------------- snapshot ----

void PinProbe::pin_changed(int pin, emu::PinState state, uint64_t cycles) {
    (void)pin;
    level_ = state;
    last_change_ = cycles;
}

std::string PinProbe::describe() const {
    switch (level_) {
    case emu::PinState::High: return "high";
    case emu::PinState::Low: return "low";
    case emu::PinState::Floating: break;
    }
    return "floating";
}

namespace {

// The label a pin is known by on the board, which is not always its number: an
// analog input is asked for as A0 whatever digital number it also answers to,
// and the two serial pins are marked RX and TX on every silkscreen.
std::string pin_label(const avr::AvrDevice& device, int pin) {
    for (size_t i = 0; i < device.analog.size(); ++i) {
        if (!device.analog[i].exists) continue;
        if (device.analog_pin_base != 0 &&
            pin == int(device.analog_pin_base) + int(i))
            return "A" + std::to_string(i);
    }
    if (device.udr != 0 && pin == 0) return "RX";
    if (device.udr != 0 && pin == 1) return "TX";
    return "D" + std::to_string(pin);
}

} // namespace

Snapshot build_snapshot(const emu::Machine& machine, const SnapshotInputs& in) {
    Snapshot snap;
    snap.sketch = in.sketch;
    snap.serial = in.serial;
    snap.running = in.running;
    snap.halted = in.halted;
    snap.fault = in.fault;
    snap.core_name = machine.description();

    const emu::State& state = machine.state();
    snap.cycles = machine.cycles();
    for (int i = 0; i < 32; ++i) snap.r[i] = state.r[i];
    snap.pc = state.pc;
    snap.sp = state.sp;

    // SREG is materialised here rather than read out of the machine because
    // the machine does not keep it packed: the flags are separate bytes so the
    // host's own flags can be spilled into them one setcc at a time, and SREG
    // is assembled only when something asks for it. This is something asking.
    snap.sreg = uint8_t((state.flag_c ? 1u << 0 : 0u) | (state.flag_z ? 1u << 1 : 0u) |
                        (state.flag_n ? 1u << 2 : 0u) | (state.flag_v ? 1u << 3 : 0u) |
                        (state.flag_s ? 1u << 4 : 0u) | (state.flag_h ? 1u << 5 : 0u) |
                        (state.flag_t ? 1u << 6 : 0u) | (state.flag_i ? 1u << 7 : 0u));

    snap.host_seconds = in.host_seconds;

    const int f_cpu = in.board ? in.board->f_cpu : 0;
    if (in.board) snap.board_name = in.board->name;
    if (f_cpu > 0) snap.sketch_seconds = double(snap.cycles) / double(f_cpu);
    if (in.host_seconds > 0.0)
        snap.mips = double(snap.cycles) / in.host_seconds / 1e6;

    // The parts, described by themselves. describe() renders state a part is
    // already holding, so asking costs nothing and dates nothing.
    if (in.wiring) {
        for (const Wiring& w : *in.wiring) {
            if (!w.part) continue;
            PartView view;
            view.kind = w.part->kind();
            view.pin = w.pin;
            view.description = w.part->describe();
            snap.parts.push_back(view);
        }
    }

    const avr::AvrDevice* device =
        in.board ? avr::find_device(in.board->mcu) : nullptr;
    if (!device) return snap;

    for (size_t pin = 0; pin < device->pins.size(); ++pin) {
        PinView view;
        view.number = int(pin);
        view.exists = device->pins[pin].pin_reg != 0;
        if (!view.exists) {
            // A gap in the numbering is reported rather than skipped, so the
            // board view can leave a hole where the part has no pin instead of
            // renumbering everything after it.
            snap.pins.push_back(view);
            continue;
        }
        view.label = pin_label(*device, int(pin));

        if (in.probes && pin < in.probes->size()) {
            const emu::PinState level = (*in.probes)[pin].level();
            view.high = level == emu::PinState::High;
        }

        if (in.wiring) {
            for (const Wiring& w : *in.wiring) {
                if (w.pin != int(pin) || !w.part) continue;
                if (view.part.empty()) view.part = w.part->kind();
                if (w.part->drive(int(pin)) != emu::PinState::Floating)
                    view.driven_externally = true;
            }
        }

        // is_output and pullup are left false, and that is a gap rather than a
        // finding: neither can be observed through Machine's interface. A pin
        // reads high both when the sketch drives it high and when it is an
        // input with its pull-up on, and nothing on the interface distinguishes
        // the two -- DDRx and PORTx live in a peripheral the machine does not
        // expose and are not in the SRAM that State does. Guessing would put a
        // plausible wrong direction on the screen, which is worse here than an
        // absent one, so the honest answer is the level and no claim about the
        // driver. A `virtual PinState pin_level(int) const` plus direction and
        // pull-up on Machine would fill these in and retire the probes below.
        snap.pins.push_back(view);
    }

    return snap;
}

// --------------------------------------------------------------- entry ----

namespace {

// One frame: draw the snapshot into the layout and put it on the terminal.
void draw_frame(hike_context* ctx, const AppLayout& layout, const Snapshot& snap,
                const AppState& state, const Running& run) {
    hike_clear(ctx);

    if (layout.status.w > 0 && layout.status.h > 0)
        draw_status(ctx, layout.status, snap);
    if (layout.board.w > 0 && layout.board.h > 0)
        draw_board(ctx, layout.board, snap);
    if (layout.parts.w > 0 && layout.parts.h > 0)
        draw_parts(ctx, layout.parts, snap);
    if (layout.cpu.w > 0 && layout.cpu.h > 0)
        draw_cpu(ctx, layout.cpu, snap);
    if (layout.serial.w > 0 && layout.serial.h > 0)
        draw_serial(ctx, layout.serial, snap, state.serial_scroll);

    if (layout.help.w > 0 && layout.help.h > 0) {
        // The requested speed belongs here rather than in the status line,
        // which reports the measured one. Showing both, side by side, is how a
        // user sees that a request for real time is not being met -- and when
        // it is not, this says so in words rather than leaving them to compare
        // two numbers and work it out.
        std::string line = unthrottled(state) ? "speed full"
                                              : "speed " + format_speed(current_speed(state));
        if (!run.keeping_up && !unthrottled(state))
            line += " (host cannot keep up)";
        line += "  ";
        line += key_help_text();

        hike_style style = hike_style_default();
        style.attrs = HIKE_DIM;
        hike_label(ctx, layout.help, line.c_str(), HIKE_ALIGN_LEFT, style);
    }

    hike_present(ctx);
}

int session(hike_context* ctx, emu::Machine& machine, const Board& board,
            const std::vector<Wiring>& wiring, const std::vector<PinProbe>& probes,
            const std::string& sketch, std::string& error) {
    AppState state;
    Running run;

    // Whatever the sketch printed before the screen existed is already in the
    // USART, and dropping it would lose the first line of most sketches.
    run.serial = machine.take_serial_output();

    for (;;) {
        AppLayout layout = compute_layout(
            hike_rect{0, 0, hike_width(ctx), hike_height(ctx)});
        state.serial_rows = layout.serial.h > 0 ? layout.serial.h : 0;
        state.serial_lines = count_lines(run.serial);
        if (state.serial_scroll > max_serial_scroll(state))
            state.serial_scroll = max_serial_scroll(state);

        SnapshotInputs inputs;
        inputs.board = &board;
        inputs.wiring = &wiring;
        inputs.probes = &probes;
        inputs.sketch = sketch;
        inputs.serial = run.serial;
        inputs.host_seconds = run.host_seconds;
        inputs.running = !state.paused && !run.halted && run.fault.empty();
        inputs.halted = run.halted;
        inputs.fault = run.fault;

        const Snapshot snap = build_snapshot(machine, inputs);
        draw_frame(ctx, layout, snap, state, run);

        // A machine that has stopped will not stop again, so waiting
        // indefinitely costs nothing and spinning would cost a core.
        const bool idle = state.paused || run.halted || !run.fault.empty();
        int timeout_ms = idle ? -1 : int(run.sleep_seconds * 1000.0);
        if (timeout_ms > 0 && timeout_ms > int(kFrameSeconds * 1000.0) + 1)
            timeout_ms = int(kFrameSeconds * 1000.0) + 1;

        hike_event ev;
        bool quit = false;
        while (hike_poll(ctx, &ev, timeout_ms)) {
            timeout_ms = 0;   // drain whatever else is waiting without waiting again

            if (ev.kind == HIKE_EVENT_RESIZE) {
                // libhike reports the new size and leaves the grid at the old
                // one, so the buffers are replaced here. Nothing is sampled
                // from the machine to do it: the next frame redraws the same
                // snapshot at the new size, which is what keeps a resize from
                // perturbing the timing on display.
                hike_buffers_free(ctx);
                if (hike_buffers_alloc(ctx, ev.size.w, ev.size.h) != HIKE_OK) {
                    error = "the terminal was resized to " +
                            std::to_string(ev.size.w) + " by " +
                            std::to_string(ev.size.h) +
                            " and there was not enough memory for a screen that size";
                    return 1;
                }
                hike_invalidate(ctx);
                continue;
            }
            if (ev.kind != HIKE_EVENT_KEY) continue;

            switch (apply_key(state, ev.key)) {
            case KeyAction::None:
                break;
            case KeyAction::Quit:
                quit = true;
                break;
            case KeyAction::Reset:
                machine.reset();
                run = Running{};
                // Reset is a reset of the measurements too. Carrying the old
                // host seconds over would report a speed for a run that is no
                // longer the one on the screen.
                break;
            case KeyAction::StepInstruction:
                // One cycle is enough to make the core execute exactly one
                // instruction: it runs until the cycle count has moved, and
                // the cheapest instruction moves it by one.
                advance(machine, board, state, run, 1);
                break;
            case KeyAction::StepSlice:
                advance(machine, board, state, run,
                        pace_cycles(board.f_cpu, current_speed(state), kFrameSeconds,
                                    run.measured_cps));
                break;
            }
            if (quit) break;
        }
        if (quit) break;

        if (state.paused || run.halted || !run.fault.empty()) continue;

        advance(machine, board, state, run,
                pace_cycles(board.f_cpu, current_speed(state), kFrameSeconds,
                            run.measured_cps));
    }

    if (!run.fault.empty()) {
        error = run.fault;
        return 1;
    }
    return 0;
}

} // namespace

int run_emulate_app(emu::Machine& machine, const Board& board,
                    const std::vector<Wiring>& wiring, const std::string& sketch,
                    std::string& error) {
    error.clear();

    const avr::AvrDevice* device = avr::find_device(board.mcu);
    if (!device) {
        error = "ardio has no device description for " + board.mcu +
                ", so it cannot show the pins of " + board.name;
        return 1;
    }

    // A probe on every pin the part brings out. See PinProbe in app.h for why
    // this is how the levels are read; the vector is sized to the whole pin
    // table so a probe can be indexed by digital pin number, and it must not
    // be resized afterwards because the machine holds pointers into it.
    std::vector<PinProbe> probes(device->pins.size());
    for (size_t pin = 0; pin < device->pins.size(); ++pin)
        if (device->pins[pin].pin_reg != 0) machine.wire(int(pin), &probes[pin]);

    hike_options options = hike_default_options();
    options.alternate_screen = true;   // leave the user's scrollback as it was
    options.hide_cursor = true;
    options.mouse = false;             // nothing here is clickable, so nothing needs a click
    options.bracketed_paste = false;

    hike_context* ctx = nullptr;
    hike_status status = hike_init(&ctx, &options);
    if (status != HIKE_OK) {
        error = std::string("cannot start the emulator's screen: ") +
                hike_status_text(status);
        return 1;
    }

    Session restore(ctx);
    try {
        return session(ctx, machine, board, wiring, probes, sketch, error);
    } catch (const std::exception& e) {
        // The terminal is restored by `restore` on the way out of this frame,
        // before anything the caller prints reaches a screen that would
        // otherwise still be in raw mode on the alternate buffer.
        error = std::string("the emulator's screen failed: ") + e.what();
        return 1;
    } catch (...) {
        error = "the emulator's screen failed for an unknown reason";
        return 1;
    }
}

} // namespace ardio::tui
