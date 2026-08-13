#pragma once
#include "hike/hike.h"
#include <cstdint>
#include <string>
#include <vector>

// The views for `ardio emulate --tui`: a running board you can watch.
//
// Everything here draws from a snapshot rather than from the live machine.
// That split is the point of this header. Drawing must never advance the
// emulator, read a peripheral, or ask a part a question, because doing any of
// those from inside a draw would change what is being drawn -- a peripheral
// access is dated at the current cycle, so rendering the screen would perturb
// the timing the screen exists to show. Take a snapshot between runs, then
// draw it as many times as you like.
namespace ardio::tui {

// What one pin is doing, as the board view needs it.
struct PinView {
    int number = 0;          // Arduino digital pin number
    bool exists = false;     // false for a gap in the numbering
    bool is_output = false;  // the sketch set DDR for it
    bool high = false;
    bool pullup = false;
    bool driven_externally = false;  // a part is fighting or feeding it
    std::string label;       // "D13", "A0", "TX"
    std::string part;        // kind of the part wired here, empty if none
};

struct PartView {
    std::string kind;
    int pin = 0;
    std::string description;  // the part's own describe()
};

// A single moment of the machine, complete enough to draw the whole screen.
struct Snapshot {
    // --- identity ---
    std::string board_name;
    std::string core_name;    // which execution core, for the status line
    std::string sketch;

    // --- time ---
    uint64_t cycles = 0;
    double sketch_seconds = 0.0;  // cycles / f_cpu
    double host_seconds = 0.0;    // wall time actually spent
    double mips = 0.0;            // measured, not claimed

    // --- cpu ---
    uint8_t r[32] = {};
    uint16_t pc = 0;          // word address
    uint16_t sp = 0;
    uint8_t sreg = 0;         // packed, for the ITHSVNZC display

    // --- io ---
    std::vector<PinView> pins;
    std::vector<PartView> parts;

    // Serial output accumulated so far. Held whole rather than as a delta so
    // the view can scroll back, which is the first thing anyone wants when a
    // sketch prints something and it leaves the screen.
    std::string serial;

    // --- state ---
    bool running = false;
    bool halted = false;
    std::string fault;        // non-empty when the sketch faulted
};

// Draws the chip with its pins around it, each showing direction and level.
//
// This is the view that makes an emulator feel like a board rather than a log:
// seeing D13 go high at the same moment the LED lights is the thing a serial
// print cannot tell you.
void draw_board(hike_context* ctx, hike_rect area, const Snapshot& snap);

// One row per wired part, showing what it has observed.
void draw_parts(hike_context* ctx, hike_rect area, const Snapshot& snap);

// Registers, PC, SP and the status register's flags spelled out.
void draw_cpu(hike_context* ctx, hike_rect area, const Snapshot& snap);

// The sketch's serial output, scrolled to `scroll` lines from the bottom.
void draw_serial(hike_context* ctx, hike_rect area, const Snapshot& snap,
                 int scroll);

// The status line: board, core, elapsed sketch time, measured speed, and
// whether the machine is running, paused, halted or faulted.
void draw_status(hike_context* ctx, hike_rect area, const Snapshot& snap);

} // namespace ardio::tui
