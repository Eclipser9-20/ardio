// The views for `ardio emulate --tui`.
//
// Every function here is a pure function of a Snapshot and a rect. Nothing in
// this file may touch a live machine, and that is not a stylistic preference:
// a peripheral dates an access from the cycle it was last advanced to, so a
// draw that read a port would move the very timing the screen exists to show,
// and the screen would then be reporting on itself. Drawing from a snapshot
// makes a redraw free and repeatable -- the same snapshot drawn twice gives
// the same pixels, which is also what makes any of this testable.
//
// Two rules run through all five views.
//
// The first is that state is never carried by colour alone. libhike reduces a
// truecolour style down to 16 colours or to none at all, and a design that
// distinguishes an output pin from an input one only by hue becomes an
// unreadable grid of identical characters on a monochrome terminal. So every
// distinction that matters is carried by a glyph or by an attribute first, and
// colour is added on top for the terminals that have it. The direction of a
// pin is '>' or '<' before it is cyan or yellow; a set flag is upper case
// before it is bold.
//
// The second is that a view must survive any rect it is handed. A terminal can
// be any size, panels are laid out by someone else, and a draw that escapes
// its rect corrupts a neighbour that has already drawn correctly. Each entry
// point pushes a clip for its area so an arithmetic slip cannot become another
// panel's problem, and each one decides deliberately what it becomes when it
// does not fit rather than letting the clip decide by cutting a diagram in
// half. The board view, given ten columns, does not show a broken chip: it
// shows a plain list of pins, and below a certain width just the levels, which
// is little but is honest about being little.

#include "ardio/tui/emulate_view.h"

#include "hike/widgets.h"

#include <cstdio>
#include <string>
#include <vector>

namespace ardio::tui {
namespace {

// The palette. Indexed rather than RGB because these are already the sixteen
// colours every terminal has, so the reduction libhike would do is a no-op and
// what is written here is what a poor terminal shows.
const hike_color kDim      = hike_indexed(8);
const hike_color kText     = hike_default_color();
const hike_color kHigh     = hike_indexed(11);  // a driven-high output
const hike_color kLow      = hike_indexed(4);
const hike_color kInput    = hike_indexed(6);
const hike_color kPart     = hike_indexed(10);
const hike_color kExternal = hike_indexed(13);
const hike_color kFault    = hike_indexed(9);
const hike_color kFrame    = hike_indexed(8);
const hike_color kNone     = hike_default_color();

hike_style plain(hike_color fg, uint16_t attrs = 0) {
    return hike_style_make(fg, kNone, attrs);
}

// Truncates to a column budget. Everything drawn here is ASCII, so a byte is a
// column and this can cut where it likes; a view that showed user text with
// multi-byte characters would have to walk code points instead.
std::string fit(const std::string& s, int width) {
    if (width <= 0) return std::string();
    if (int(s.size()) <= width) return s;
    return s.substr(0, size_t(width));
}

int draw(hike_context* ctx, int x, int y, const std::string& s,
         hike_color fg, uint16_t attrs = 0) {
    return hike_text(ctx, x, y, s.c_str(), fg, kNone, attrs);
}

std::string hex8(uint8_t v) {
    char buf[3];
    std::snprintf(buf, sizeof buf, "%02X", unsigned(v));
    return buf;
}

std::string hex16(uint16_t v) {
    char buf[5];
    std::snprintf(buf, sizeof buf, "%04X", unsigned(v));
    return buf;
}

std::string number(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

// ------------------------------------------------------------------ pins ---

// How a pin reads at a glance, as glyphs.
//
//   direction   '>' the sketch drives it, '<' it is an input, '.' absent
//   level       '1' or '0'
//   modifier    '=' something outside is driving or fighting it,
//               'u' its pull-up is on, ' ' neither
//
// The three are independent and each is a character, so output-high,
// output-low and input differ in the grid itself and not merely in colour.
// Which of '=' and 'u' wins when both apply is decided in favour of '=': a
// pull-up is something the sketch asked for and can be read off the code,
// while an external driver is the thing the sketch cannot see and is the
// reason to be watching the screen at all.

char pin_direction(const PinView& p) {
    if (!p.exists) return '.';
    return p.is_output ? '>' : '<';
}

char pin_level(const PinView& p) { return p.high ? '1' : '0'; }

char pin_modifier(const PinView& p) {
    if (p.driven_externally) return '=';
    if (p.pullup) return 'u';
    return ' ';
}

hike_color pin_color(const PinView& p) {
    if (!p.exists) return kDim;
    if (p.driven_externally) return kExternal;
    if (p.is_output) return p.high ? kHigh : kLow;
    return kInput;
}

// Bold for a pin that is doing something -- an output driving high, or a pin
// an outside part is holding. On a terminal with no colour at all this is the
// only thing left that separates the interesting rows from the quiet ones, so
// it is chosen for exactly the states a person is watching for.
uint16_t pin_attrs(const PinView& p) {
    if (!p.exists) return HIKE_DIM;
    if (p.driven_externally) return HIKE_BOLD;
    return (p.is_output && p.high) ? HIKE_BOLD : 0;
}

// The widest label in a set, so a column of pins lines up its levels. Pins
// arrive labelled by the board and the labels are not all the same length --
// "D9" against "A0" against "SCK" -- and levels that do not line up cannot be
// scanned down a column.
int label_width(const std::vector<PinView>& pins) {
    int w = 2;
    for (const auto& p : pins) {
        if (int(p.label.size()) > w) w = int(p.label.size());
    }
    return w;
}

// One pin as text, laid out right-to-left or left-to-right depending on which
// side of the chip it hangs off. `budget` is the columns available; the part
// name is dropped first and the label second, because the level is the thing
// being watched and the name of the part is recoverable from the parts view.
std::string pin_text(const PinView& p, int label_cols, int budget, bool mirrored) {
    std::string state;
    state.push_back(pin_direction(p));
    state.push_back(pin_level(p));
    char mod = pin_modifier(p);

    std::string label = p.label;
    if (int(label.size()) < label_cols) {
        label.append(size_t(label_cols - int(label.size())), ' ');
    }

    // A part is marked with '*' and then named. The marker is separate from
    // the name so that a narrow column can drop the name and still say that
    // something is wired here, which is the half of the fact that changes how
    // a level should be read.
    std::string part;
    if (!p.part.empty()) part = std::string("*") + p.part;

    std::string full;
    if (mirrored) {
        // The left-hand column reads outward from the chip, so the state sits
        // against the chip and the label and part trail away from it.
        full = part;
        if (!full.empty()) full += ' ';
        full += label;
        full += ' ';
        full.push_back(mod);
        full += state;
    } else {
        full = label;
        full += ' ';
        full += state;
        full.push_back(mod);
        if (!part.empty()) full += " " + part;
    }
    if (int(full.size()) <= budget) return full;

    // Too wide: drop the part name, keeping its marker.
    std::string shorter;
    std::string mark = p.part.empty() ? std::string() : std::string("*");
    if (mirrored) {
        shorter = mark;
        if (!shorter.empty()) shorter += ' ';
        shorter += label;
        shorter += ' ';
        shorter.push_back(mod);
        shorter += state;
    } else {
        shorter = label;
        shorter += ' ';
        shorter += state;
        shorter.push_back(mod);
        shorter += mark;
    }
    if (int(shorter.size()) <= budget) return shorter;

    // Narrower still: the state and the part marker, no label.
    std::string bare = state;
    bare.push_back(mod);
    if (!mark.empty()) bare += mark;
    if (int(bare.size()) <= budget) return bare;

    // Last: direction and level, then level alone. A single column showing
    // '1' or '0' is very little, but it is true, and it is the fact the board
    // view exists for.
    std::string pair = state;
    if (int(pair.size()) <= budget) return pair;
    return std::string(1, pin_level(p));
}

void draw_pin_list(hike_context* ctx, hike_rect area,
                   const std::vector<PinView>& pins) {
    int label_cols = label_width(pins);
    int rows = area.h;
    int shown = int(pins.size());
    bool truncated = false;
    if (shown > rows) {
        // Keep the last row for the count of what is missing, so a short panel
        // says how much it is hiding instead of quietly ending.
        shown = rows > 0 ? rows - 1 : 0;
        truncated = true;
    }
    for (int i = 0; i < shown; ++i) {
        const PinView& p = pins[size_t(i)];
        draw(ctx, area.x, area.y + i,
             fit(pin_text(p, label_cols, area.w, false), area.w),
             pin_color(p), pin_attrs(p));
    }
    if (truncated && rows > 0) {
        int hidden = int(pins.size()) - shown;
        std::string more = "+" + std::to_string(hidden) + " more";
        draw(ctx, area.x, area.y + rows - 1, fit(more, area.w), kDim, HIKE_DIM);
    }
}

// ----------------------------------------------------------------- lines ---

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    // A trailing fragment with no newline is still a line: a sketch that has
    // printed half a line has printed something, and hiding it until the
    // newline arrives makes the view look frozen exactly when it is not.
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

} // namespace

// ----------------------------------------------------------------- board ---

// The board is drawn as a chip in the middle with its pins hanging off both
// sides, odd-numbered rows to the right, because that is the shape of the
// thing on the desk and recognising it is the whole point. The alternative --
// a table of pins -- is what this view degrades to, and the degradation is
// visibly a fallback rather than the design.
void draw_board(hike_context* ctx, hike_rect area, const Snapshot& snap) {
    if (!ctx || area.w <= 0 || area.h <= 0) return;
    hike_push_clip(ctx, area);

    const std::vector<PinView>& pins = snap.pins;
    if (pins.empty()) {
        hike_label(ctx, area, "no pins", HIKE_ALIGN_LEFT, plain(kDim, HIKE_DIM));
        hike_pop_clip(ctx);
        return;
    }

    int labels = label_width(pins);
    // A pin needs its label, a space, direction, level and modifier. The chip
    // needs enough width to name the board legibly. Below the sum of those the
    // two-sided drawing stops being a picture of anything.
    int side = labels + 4;
    const int kChip = 11;
    int half = (int(pins.size()) + 1) / 2;
    bool wide_enough = area.w >= side * 2 + kChip;
    bool tall_enough = area.h >= half + 2;

    if (!wide_enough || !tall_enough) {
        draw_pin_list(ctx, area, pins);
        hike_pop_clip(ctx);
        return;
    }

    int left_w = (area.w - kChip) / 2;
    int right_x = area.x + left_w + kChip;
    int right_w = area.w - left_w - kChip;
    int chip_x = area.x + left_w;

    // The chip body. Drawn with a box rather than a fill so it keeps its edges
    // when there is no colour, and titled with the board so the view answers
    // "which board is this" without the status line.
    hike_rect chip{chip_x, area.y, kChip, area.h};
    hike_style frame = plain(kFrame);
    hike_rect inner = hike_box(ctx, chip, HIKE_BORDER_SINGLE, nullptr, frame);
    if (inner.w > 0 && inner.h > 0) {
        std::string name = fit(snap.board_name.empty() ? "board" : snap.board_name,
                               inner.w);
        hike_rect title{inner.x, inner.y + inner.h / 2, inner.w, 1};
        hike_label(ctx, title, name.c_str(), HIKE_ALIGN_CENTER,
                   plain(kText, HIKE_BOLD));
    }

    for (size_t i = 0; i < pins.size(); ++i) {
        const PinView& p = pins[i];
        bool left = (i % 2) == 0;
        int row = area.y + int(i / 2);
        if (row >= area.y + area.h) break;

        if (left) {
            std::string text = pin_text(p, labels, left_w - 1, true);
            // Right-aligned so the state characters sit against the chip and
            // the column of levels reads straight down beside the body.
            int x = chip_x - 1 - int(text.size());
            if (x < area.x) x = area.x;
            draw(ctx, x, row, fit(text, chip_x - 1 - x), pin_color(p), pin_attrs(p));
            // The leg. A pin with something wired to it gets a doubled line,
            // so an attached part is visible in the diagram and not only in
            // the text.
            draw(ctx, chip_x - 1, row, p.part.empty() ? "-" : "=",
                 p.part.empty() ? kFrame : kPart,
                 p.part.empty() ? uint16_t(HIKE_DIM) : uint16_t(HIKE_BOLD));
        } else {
            draw(ctx, right_x, row, p.part.empty() ? "-" : "=",
                 p.part.empty() ? kFrame : kPart,
                 p.part.empty() ? uint16_t(HIKE_DIM) : uint16_t(HIKE_BOLD));
            std::string text = pin_text(p, labels, right_w - 1, false);
            draw(ctx, right_x + 1, row, fit(text, right_w - 1),
                 pin_color(p), pin_attrs(p));
        }
    }

    hike_pop_clip(ctx);
}

// ----------------------------------------------------------------- parts ---

void draw_parts(hike_context* ctx, hike_rect area, const Snapshot& snap) {
    if (!ctx || area.w <= 0 || area.h <= 0) return;
    hike_push_clip(ctx, area);

    if (snap.parts.empty()) {
        hike_label(ctx, area, "no parts wired", HIKE_ALIGN_LEFT,
                   plain(kDim, HIKE_DIM));
        hike_pop_clip(ctx);
        return;
    }

    int rows = area.h;
    int shown = int(snap.parts.size());
    bool truncated = false;
    if (shown > rows) {
        shown = rows > 0 ? rows - 1 : 0;
        truncated = true;
    }

    for (int i = 0; i < shown; ++i) {
        const PartView& part = snap.parts[size_t(i)];
        int x = area.x;
        // The pin comes first: a part is identified by where it is wired at
        // least as much as by what it is, and two of the same kind are told
        // apart only by the pin.
        std::string where = "D" + std::to_string(part.pin);
        x += draw(ctx, x, area.y + i, fit(where, area.x + area.w - x), kText,
                  HIKE_BOLD);
        if (x < area.x + area.w) x += draw(ctx, x, area.y + i, " ", kText);
        x += draw(ctx, x, area.y + i,
                  fit(part.kind, area.x + area.w - x), kPart);
        if (!part.description.empty() && x + 1 < area.x + area.w) {
            x += draw(ctx, x, area.y + i, " ", kText);
            draw(ctx, x, area.y + i,
                 fit(part.description, area.x + area.w - x), kText);
        }
    }
    if (truncated && rows > 0) {
        std::string more = "+" + std::to_string(int(snap.parts.size()) - shown)
                         + " more";
        draw(ctx, area.x, area.y + rows - 1, fit(more, area.w), kDim, HIKE_DIM);
    }

    hike_pop_clip(ctx);
}

// ------------------------------------------------------------------- cpu ---

void draw_cpu(hike_context* ctx, hike_rect area, const Snapshot& snap) {
    if (!ctx || area.w <= 0 || area.h <= 0) return;
    hike_push_clip(ctx, area);

    int y = area.y;
    int bottom = area.y + area.h;

    // PC is a word address in the snapshot, as it is in the core, and is shown
    // as one. Doubling it to a byte address here would be a kindness that
    // disagreed with every other place the number appears.
    if (y < bottom) {
        std::string line = "PC " + hex16(snap.pc) + "  SP " + hex16(snap.sp);
        draw(ctx, area.x, y, fit(line, area.w), kText, HIKE_BOLD);
        ++y;
    }

    // SREG. Each flag is its letter when set and the same letter lower case
    // when clear, so the whole register reads at a glance and still reads with
    // no colour and no attributes at all. Bold and colour are added for the
    // set ones on terminals that have them.
    if (y < bottom) {
        static const char* kNames = "ITHSVNZC";
        int x = area.x;
        int end = area.x + area.w;
        // The caption is only worth its five columns when all eight flags fit
        // after it. Where they do not, the letters take the row alone: they
        // are the data and "SREG" is decoration, and a row reading "SREG" with
        // no flags after it would be the one arrangement that says nothing.
        if (x + 13 <= end) x += draw(ctx, x, y, "SREG ", kText);
        for (int bit = 7; bit >= 0 && x < end; --bit) {
            bool set = (snap.sreg >> bit) & 1u;
            char c = kNames[7 - bit];
            std::string s(1, set ? c : char(c - 'A' + 'a'));
            x += draw(ctx, x, y, s, set ? kHigh : kDim,
                      set ? uint16_t(HIKE_BOLD) : uint16_t(HIKE_DIM));
        }
        ++y;
    }

    // The registers, in as many columns as fit. Each is "Rnn hh", six columns
    // plus a separating space. When even one column does not fit the register
    // file is simply not drawn: half a register file is worse than none,
    // because a number cut in half still looks like a number.
    const int kCell = 7;
    int cols = (area.w + 1) / kCell;
    if (cols < 1 || y >= bottom) {
        hike_pop_clip(ctx);
        return;
    }
    if (cols > 8) cols = 8;
    for (int i = 0; i < 32; ++i) {
        int row = y + i / cols;
        if (row >= bottom) break;
        int x = area.x + (i % cols) * kCell;
        char buf[8];
        std::snprintf(buf, sizeof buf, "R%02d %s", i, hex8(snap.r[i]).c_str());
        // A register holding zero is the uninteresting case and by far the
        // common one, so it is dimmed and the rest stand out without anything
        // having to blink.
        bool zero = snap.r[i] == 0;
        draw(ctx, x, row, fit(buf, area.x + area.w - x), zero ? kDim : kText,
             zero ? uint16_t(HIKE_DIM) : uint16_t(0));
    }

    hike_pop_clip(ctx);
}

// ---------------------------------------------------------------- serial ---

// `scroll` counts lines back from the bottom, so zero is the newest output.
// That is the right default because a sketch that has just printed something
// has printed it at the end, and a view anchored at the top would show the
// first line of a run forever.
void draw_serial(hike_context* ctx, hike_rect area, const Snapshot& snap,
                 int scroll) {
    if (!ctx || area.w <= 0 || area.h <= 0) return;
    hike_push_clip(ctx, area);

    std::vector<std::string> lines = split_lines(snap.serial);
    int count = int(lines.size());
    if (count == 0) {
        hike_label(ctx, area, "no output yet", HIKE_ALIGN_LEFT,
                   plain(kDim, HIKE_DIM));
        hike_pop_clip(ctx);
        return;
    }

    // Clamped rather than rejected: the caller's scroll is a user's key press
    // held down, and the honest response to scrolling past the top is to stop
    // at the top, not to blank the panel.
    int max_scroll = count - area.h;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll < 0) scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;

    int first = count - area.h - scroll;
    if (first < 0) first = 0;

    for (int i = 0; i < area.h; ++i) {
        int idx = first + i;
        if (idx >= count) break;
        draw(ctx, area.x, area.y + i, fit(lines[size_t(idx)], area.w), kText);
    }

    hike_pop_clip(ctx);
}

// ---------------------------------------------------------------- status ---

void draw_status(hike_context* ctx, hike_rect area, const Snapshot& snap) {
    if (!ctx || area.w <= 0 || area.h <= 0) return;
    hike_push_clip(ctx, area);

    bool faulted = !snap.fault.empty();
    std::string state = faulted   ? "FAULTED"
                      : snap.halted ? "HALTED"
                      : snap.running ? "RUNNING"
                                     : "PAUSED";

    // The state word comes first and is drawn before anything else can be cut
    // off, because it is the one field that is worth a column when only one
    // column is left. A faulted run is reversed as well as coloured: a fault
    // must not be able to look like a normal run on a terminal that dropped
    // the colour.
    hike_color state_fg = faulted ? kFault : (snap.running ? kPart : kText);
    uint16_t state_attrs = faulted ? uint16_t(HIKE_BOLD | HIKE_REVERSE)
                                   : uint16_t(HIKE_BOLD);

    int x = area.x;
    int end = area.x + area.w;
    x += draw(ctx, x, area.y, fit(state, end - x), state_fg, state_attrs);

    // A fault message is worth more than any of the timing fields, so it is
    // placed next and the rest are allowed to fall off the end.
    if (faulted) {
        if (x + 2 < end) {
            x += draw(ctx, x, area.y, " ", kText);
            x += draw(ctx, x, area.y, fit("fault: " + snap.fault, end - x),
                      kFault, HIKE_BOLD);
        }
        hike_pop_clip(ctx);
        return;
    }

    std::string rest = "  " + snap.board_name + "  " + snap.core_name
                     + "  t=" + number(snap.sketch_seconds, 3) + "s"
                     + "  " + number(snap.mips, 2) + " MIPS";
    if (!snap.sketch.empty()) rest += "  " + snap.sketch;
    draw(ctx, x, area.y, fit(rest, end - x), kText);

    hike_pop_clip(ctx);
}

} // namespace ardio::tui
