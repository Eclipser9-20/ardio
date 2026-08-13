#pragma once
#include "ardio/board.h"
#include "ardio/emu/machine.h"
#include "ardio/tui/emulate_view.h"
#include "hike/hike.h"

#include <cstdint>
#include <string>
#include <vector>

// The application shell behind `ardio emulate --tui`: the run loop, the
// pacing, the keyboard and the layout that the views in emulate_view.h are
// drawn into.
//
// Everything in this header except run_emulate_app is a pure function or a
// plain struct, and that split is deliberate rather than tidiness. There is no
// terminal in CI, so any logic that lives inside the run loop is logic that
// cannot be tested: the pacing arithmetic, the key handling and the layout are
// therefore stated here as values in and values out, and the run loop's only
// remaining job is to call them in order and to talk to libhike.
namespace ardio::tui {

// A part as the caller wired it.
//
// This is passed in rather than read back out of the Machine because a Machine
// does not offer its wiring: Machine::wire takes a Part and keeps a borrowed
// pointer, and nothing on the interface hands the list back. The caller that
// did the wiring has it, so it supplies it, and the parts must outlive the
// call for the same reason the Machine's own borrow requires.
struct Wiring {
    int pin = 0;
    emu::Part* part = nullptr;
};

// ------------------------------------------------------------- pacing ----

// The emulator runs far faster than the silicon it emulates, so a sketch left
// unthrottled finishes a minute of behaviour in a second or two and shows the
// user nothing but a flicker. The default is therefore real time, and these
// are the multiples of it the speed key steps through. Zero is the unthrottled
// end of the ladder: it is a speed the host picks, not one we ask for, which
// is why it cannot be written as a number here.
constexpr double kSpeedLadder[] = {0.05, 0.1, 0.25, 0.5, 1.0,
                                   2.0,  5.0, 10.0, 25.0, 50.0, 0.0};
constexpr int kSpeedCount = int(sizeof(kSpeedLadder) / sizeof(kSpeedLadder[0]));
constexpr int kSpeedRealTime = 4;   // the index of 1.0, and the default

// The wall-clock budget for one frame. It sets how often the screen is redrawn
// and therefore how finely the pacing can be steered; a slice this long is
// also short enough that a pause takes effect without a visible delay.
constexpr double kFrameSeconds = 1.0 / 30.0;

// How many guest cycles a frame should cover.
//
// At a requested speed this is simply the number of cycles that much sketch
// time contains, whether or not the host can deliver them -- asking for fewer
// because the host is slow would quietly turn a request for real time into a
// request for whatever we happen to manage, which is the thing the measured
// speed exists to expose. At the unthrottled end there is no requested rate to
// convert, so the slice is sized from the throughput actually observed so far
// (`measured_cycles_per_second`, zero when nothing has been measured yet) to
// keep the frame near its budget.
uint64_t pace_cycles(int f_cpu, double speed, double frame_seconds,
                     double measured_cycles_per_second);

// What to do once a slice has been run.
struct PaceResult {
    double sleep_seconds = 0.0;  // hold back this long to keep the requested rate
    bool keeping_up = true;      // false when the host took longer than the sketch would have
};

// Compares the wall time a slice actually took against the sketch time it
// represents. Sleeping the difference is what makes the run real time; the
// difference coming out negative is what makes it honest, because that is the
// host failing to keep up and it is reported rather than absorbed.
PaceResult pace_after_run(int f_cpu, double speed, uint64_t cycles_run,
                          double spent_seconds);

// ----------------------------------------------------------- keyboard ----

// What the run loop must do about a key. The state that a key changes on its
// own -- paused, speed, scroll -- is written straight into the AppState;
// anything that needs the Machine comes back as one of these, because a pure
// function is not the place to run an emulator.
enum class KeyAction {
    None,
    Quit,
    Reset,
    StepInstruction,   // advance until the cycle count moves, so exactly one instruction
    StepSlice,         // advance by one frame's worth of cycles
};

struct AppState {
    bool paused = false;
    int speed_index = kSpeedRealTime;

    // Lines of serial output scrolled back from the bottom. Held as a distance
    // from the end rather than an absolute line so that output arriving while
    // the user is reading does not drag the view: at zero the pane follows the
    // tail, and above zero it stays where it was put.
    int serial_scroll = 0;

    // What the last frame knew about the serial pane, which is what the scroll
    // has to be clamped against. Both are updated by the run loop after it has
    // laid out the frame, so a resize changes what a page of scrolling means.
    int serial_lines = 0;
    int serial_rows = 0;
};

double current_speed(const AppState& state);
bool unthrottled(const AppState& state);

// The furthest back the serial pane can be scrolled: enough to bring the first
// line into view and not one line further. Scrolling past the start of a log
// shows blank space and reads as lost output.
int max_serial_scroll(const AppState& state);

// Applies one key. Pure: the only thing it touches is `state`.
KeyAction apply_key(AppState& state, const hike_key_event& key);

// The bindings, in the order they are shown. A TUI whose controls are not on
// the screen is a worse tool than the flag it replaced, so this is drawn every
// frame rather than hidden behind a help key.
const char* key_help_text();

// ------------------------------------------------------------- layout ----

// Where each view goes. Every rect is valid at any terminal size, including
// sizes too small to hold everything: a pane that does not fit comes back
// empty rather than negative or overlapping, and an empty pane is simply not
// drawn.
struct AppLayout {
    hike_rect status{};
    hike_rect board{};
    hike_rect cpu{};
    hike_rect parts{};
    hike_rect serial{};
    hike_rect help{};
};

AppLayout compute_layout(hike_rect screen);

// ----------------------------------------------------------- snapshot ----

// A part wired to a pin purely so that the level on it can be read back.
//
// This exists because Machine offers no way to ask what a pin is doing. The
// only thing on the interface that is told about pin levels is a Part, so the
// application wires one of these to every pin the board brings out and reads
// the levels from them. A probe drives nothing, so it is electrically inert:
// wiring one cannot change what the sketch observes. It does cost a little
// host time, since settling a pin is proportional to the number of parts on
// the board -- which is a real cost paid for a view of the pins, and the note
// in run_emulate_app's comment says what would remove it.
class PinProbe final : public emu::Part {
public:
    std::string kind() const override { return "probe"; }
    void pin_changed(int pin, emu::PinState state, uint64_t cycles) override;
    std::string describe() const override;

    emu::PinState level() const { return level_; }
    uint64_t last_change() const { return last_change_; }

private:
    emu::PinState level_ = emu::PinState::Floating;
    uint64_t last_change_ = 0;
};

// Everything a snapshot needs that the Machine does not hold: what the
// application has measured, what it has accumulated, and how the pins are
// being observed.
struct SnapshotInputs {
    const Board* board = nullptr;
    const std::vector<Wiring>* wiring = nullptr;
    const std::vector<PinProbe>* probes = nullptr;  // indexed by digital pin number
    std::string sketch;
    std::string serial;          // everything the sketch has printed so far
    double host_seconds = 0.0;   // wall time spent running, never time spent paused
    bool running = false;
    bool halted = false;
    std::string fault;
};

// Builds the moment the views draw.
//
// Nothing here advances the machine or touches a peripheral. Part::drive is
// asked what each part is putting on its pin, which is a question a part
// answers from what it already decided rather than by looking at the clock,
// and Part::describe is a rendering of state the part already holds.
Snapshot build_snapshot(const emu::Machine& machine, const SnapshotInputs& in);

// --------------------------------------------------------------- entry ----

// Runs the interactive session and returns a process exit code: 0 when the
// user quit or the sketch simply stopped, 1 when something failed, with
// `error` set to a sentence explaining what.
//
// The Machine must already have its program loaded, and every Part in `wiring`
// must already be wired to it and must outlive this call. Both are the
// caller's job because both are things the caller had to do anyway to get a
// Machine worth watching, and doing them again here would either duplicate the
// wiring or silently disagree with it.
//
// The terminal is restored on every path out, including a fault and an
// exception. libhike installs handlers for the signals it can, but a return
// through an error path is this function's own responsibility, and leaving raw
// mode set hands the user a shell that does not echo.
int run_emulate_app(emu::Machine& machine, const Board& board,
                    const std::vector<Wiring>& wiring, const std::string& sketch,
                    std::string& error);

} // namespace ardio::tui
