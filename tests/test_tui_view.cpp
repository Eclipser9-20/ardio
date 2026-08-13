// Tests for the emulate views.
//
// These assert on CELL CONTENTS, because that is the whole of what a view
// does: it is a function from a Snapshot to characters in columns, and a test
// that checked anything else -- that a function ran, that no assertion fired --
// would pass just as happily if the screen came out blank.
//
// Two things are checked repeatedly and on purpose. The first is that state is
// distinguishable WITHOUT colour: every "these two states look different"
// assertion compares characters and attributes, never the foreground, so it
// fails if a view ever starts leaning on hue alone. The second is that a view
// handed a rect smaller than it wants writes nothing outside it. Those tests
// fill the whole grid with a sentinel first and then check the sentinel
// survives everywhere except inside the rect, which catches an escape in any
// direction rather than only the one that was imagined.
//
// There is no terminal in CI, so all of this runs on a memory context.

#include "harness.h"

#include "ardio/tui/emulate_view.h"

#include "../libhike/src/hike_internal.h"

#include <string>
#include <vector>

using ardio::tui::PartView;
using ardio::tui::PinView;
using ardio::tui::Snapshot;

namespace {

struct Ctx {
    hike_context* ctx;
    explicit Ctx(int w, int h)
        : ctx(hike_context_new_memory(w, h, HIKE_COLOR_TRUE)) {}
    ~Ctx() { hike_context_free(ctx); }
    Ctx(const Ctx&) = delete;
    Ctx& operator=(const Ctx&) = delete;
    operator hike_context*() const { return ctx; }
};

// One row as a string. Non-ASCII becomes '?' so an expectation can be written
// as a readable literal; the tests that care about a specific box-drawing
// character check the code point directly.
std::string row_text(hike_context* ctx, int y, int x, int w) {
    std::string out;
    for (int i = 0; i < w; ++i) {
        uint32_t ch = hike_get_cell(ctx, x + i, y).ch;
        out.push_back(ch >= 0x20 && ch < 0x7F ? char(ch) : '?');
    }
    return out;
}

std::string whole_row(hike_context* ctx, int y, int w) {
    return row_text(ctx, y, 0, w);
}

bool row_contains(hike_context* ctx, int y, int w, const std::string& needle) {
    return whole_row(ctx, y, w).find(needle) != std::string::npos;
}

bool grid_contains(hike_context* ctx, int w, int h, const std::string& needle) {
    for (int y = 0; y < h; ++y) {
        if (row_contains(ctx, y, w, needle)) return true;
    }
    return false;
}

// Fills every cell with a sentinel the views never draw, so anything left
// after a draw is a cell that was not written.
void fill_sentinel(hike_context* ctx, int w, int h) {
    hike_cell cell{};
    cell.ch = uint32_t('#');
    cell.fg = hike_default_color();
    cell.bg = hike_default_color();
    hike_rect all{0, 0, w, h};
    hike_fill(ctx, all, cell);
}

// Checks the sentinel survives everywhere outside `area`.
bool untouched_outside(hike_context* ctx, int w, int h, hike_rect area) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (hike_rect_contains(area, x, y)) continue;
            if (hike_get_cell(ctx, x, y).ch != uint32_t('#')) return false;
        }
    }
    return true;
}

PinView make_pin(int number, const char* label, bool output, bool high) {
    PinView p;
    p.number = number;
    p.label = label;
    p.exists = true;
    p.is_output = output;
    p.high = high;
    return p;
}

// A snapshot with a handful of pins, one of which has a part on it.
Snapshot basic_snapshot() {
    Snapshot s;
    s.board_name = "uno";
    s.core_name = "jit";
    s.sketch = "blink.ino";
    s.running = true;
    s.mips = 12.5;
    s.sketch_seconds = 1.5;
    s.pins.push_back(make_pin(11, "D11", true, false));
    s.pins.push_back(make_pin(12, "D12", false, false));
    s.pins.push_back(make_pin(13, "D13", true, true));
    s.pins.back().part = "led";
    s.pins.push_back(make_pin(2, "D2", false, true));
    s.pins.back().pullup = true;
    s.parts.push_back(PartView{"led", 13, "on, 42 changes"});
    return s;
}

// The style of one cell reduced to what a monochrome terminal keeps: the
// character and the attributes. Colour is deliberately excluded, so a test
// written against this cannot be satisfied by a hue change.
struct Mono {
    uint32_t ch;
    uint16_t attrs;
    bool operator==(const Mono& o) const {
        return ch == o.ch && attrs == o.attrs;
    }
    bool operator!=(const Mono& o) const { return !(*this == o); }
};

Mono mono(hike_context* ctx, int x, int y) {
    hike_cell c = hike_get_cell(ctx, x, y);
    return Mono{c.ch, c.attrs};
}

std::string mono_row(hike_context* ctx, int y, int w) {
    // The characters only, which is what a colourless terminal shows.
    return whole_row(ctx, y, w);
}

} // namespace

// ----------------------------------------------------------------- board ---

TEST(tui_board_output_high_differs_from_low_without_colour) {
    // Two pins identical but for their level must produce different
    // characters, not merely different colours.
    Snapshot s;
    s.board_name = "uno";
    s.pins.push_back(make_pin(13, "D13", true, true));
    s.pins.push_back(make_pin(12, "D12", true, false));

    Ctx ctx(60, 10);
    ardio::tui::draw_board(ctx, hike_rect{0, 0, 60, 10}, s);

    // Left column holds pin 0 (D13, output high), right column pin 1 (D12).
    std::string row = whole_row(ctx, 0, 60);
    CHECK(row.find(">1") != std::string::npos);
    CHECK(row.find(">0") != std::string::npos);
}

TEST(tui_board_input_differs_from_output) {
    Snapshot s;
    s.board_name = "uno";
    s.pins.push_back(make_pin(13, "D13", true, true));   // output high
    s.pins.push_back(make_pin(12, "D12", false, true));  // input high

    Ctx ctx(60, 10);
    ardio::tui::draw_board(ctx, hike_rect{0, 0, 60, 10}, s);

    std::string row = whole_row(ctx, 0, 60);
    // Same level, opposite direction: the direction glyph is what separates
    // them, so both forms must be present on the row.
    CHECK(row.find(">1") != std::string::npos);
    CHECK(row.find("<1") != std::string::npos);
}

TEST(tui_board_high_output_is_bold_as_well_as_coloured) {
    Snapshot s;
    s.pins.push_back(make_pin(13, "D13", true, true));
    s.pins.push_back(make_pin(12, "D12", true, false));

    Ctx ctx(60, 10);
    ardio::tui::draw_board(ctx, hike_rect{0, 0, 60, 10}, s);

    // Find the '1' of the high output and the '0' of the low one and compare
    // attributes, which is the part of the difference a monochrome terminal
    // still shows.
    std::string row = whole_row(ctx, 0, 60);
    size_t high = row.find(">1");
    size_t low = row.find(">0");
    CHECK(high != std::string::npos);
    CHECK(low != std::string::npos);
    if (high == std::string::npos || low == std::string::npos) return;
    CHECK((mono(ctx, int(high) + 1, 0).attrs & HIKE_BOLD) != 0);
    CHECK((mono(ctx, int(low) + 1, 0).attrs & HIKE_BOLD) == 0);
}

TEST(tui_board_pin_with_part_is_distinguishable) {
    Snapshot s;
    s.board_name = "uno";
    s.pins.push_back(make_pin(13, "D13", true, true));
    s.pins.back().part = "led";
    s.pins.push_back(make_pin(12, "D12", true, true));

    Ctx ctx(60, 10);
    ardio::tui::draw_board(ctx, hike_rect{0, 0, 60, 10}, s);

    std::string row = whole_row(ctx, 0, 60);
    // The part is named, marked with '*', and its leg is doubled -- three
    // separate carriers, none of which is a colour.
    CHECK(row.find("*led") != std::string::npos);
    CHECK(row.find("=") != std::string::npos);

    // The pin without a part gets a plain leg.
    CHECK(row.find("-") != std::string::npos);
}

TEST(tui_board_external_drive_and_pullup_have_their_own_glyphs) {
    Snapshot s;
    s.pins.push_back(make_pin(2, "D2", false, false));
    s.pins.back().driven_externally = true;
    s.pins.push_back(make_pin(3, "D3", false, true));
    s.pins.back().pullup = true;

    Ctx ctx(60, 10);
    ardio::tui::draw_board(ctx, hike_rect{0, 0, 60, 10}, s);

    std::string row = whole_row(ctx, 0, 60);
    CHECK(row.find("=<0") != std::string::npos);  // left column, mirrored
    CHECK(row.find("<1u") != std::string::npos);  // right column
}

TEST(tui_board_names_the_board_in_the_chip) {
    Snapshot s = basic_snapshot();
    Ctx ctx(60, 12);
    ardio::tui::draw_board(ctx, hike_rect{0, 0, 60, 12}, s);
    CHECK(grid_contains(ctx, 60, 12, "uno"));
}

TEST(tui_board_narrow_degrades_to_a_pin_list) {
    Snapshot s = basic_snapshot();
    Ctx ctx(10, 6);
    ardio::tui::draw_board(ctx, hike_rect{0, 0, 10, 6}, s);

    // No chip, but every pin's level is still there, one per row.
    CHECK(row_contains(ctx, 0, 10, ">0"));
    CHECK(row_contains(ctx, 1, 10, "<0"));
    CHECK(row_contains(ctx, 2, 10, ">1"));
}

TEST(tui_board_very_narrow_still_shows_levels) {
    Snapshot s;
    s.pins.push_back(make_pin(13, "D13", true, true));
    s.pins.push_back(make_pin(12, "D12", true, false));

    Ctx ctx(2, 4);
    ardio::tui::draw_board(ctx, hike_rect{0, 0, 2, 4}, s);
    CHECK((whole_row(ctx, 0, 2) == std::string(">1")));
    CHECK((whole_row(ctx, 1, 2) == std::string(">0")));
}

TEST(tui_board_short_says_how_many_pins_are_hidden) {
    Snapshot s;
    for (int i = 0; i < 10; ++i) {
        s.pins.push_back(make_pin(i, ("D" + std::to_string(i)).c_str(), true, false));
    }
    Ctx ctx(12, 4);
    ardio::tui::draw_board(ctx, hike_rect{0, 0, 12, 4}, s);
    CHECK(row_contains(ctx, 3, 12, "+7 more"));
}

TEST(tui_board_with_no_pins_says_so) {
    Snapshot s;
    s.board_name = "uno";
    Ctx ctx(20, 4);
    ardio::tui::draw_board(ctx, hike_rect{0, 0, 20, 4}, s);
    CHECK(row_contains(ctx, 0, 20, "no pins"));
}

TEST(tui_board_clipped_to_a_tiny_rect_writes_nothing_outside) {
    Snapshot s = basic_snapshot();
    Ctx ctx(40, 12);
    fill_sentinel(ctx, 40, 12);
    hike_rect area{3, 2, 6, 3};
    ardio::tui::draw_board(ctx, area, s);
    CHECK(untouched_outside(ctx, 40, 12, area));
}

TEST(tui_board_clipped_when_it_would_prefer_the_chip_layout) {
    // A rect wide enough for the diagram but placed so a slip would run off
    // the right-hand edge of the grid.
    Snapshot s = basic_snapshot();
    Ctx ctx(80, 10);
    fill_sentinel(ctx, 80, 10);
    hike_rect area{10, 1, 40, 5};
    ardio::tui::draw_board(ctx, area, s);
    CHECK(untouched_outside(ctx, 80, 10, area));
}

// ----------------------------------------------------------------- parts ---

TEST(tui_parts_shows_pin_kind_and_description) {
    Snapshot s = basic_snapshot();
    Ctx ctx(40, 4);
    ardio::tui::draw_parts(ctx, hike_rect{0, 0, 40, 4}, s);
    CHECK((whole_row(ctx, 0, 40) ==
           std::string("D13 led on, 42 changes                  ")));
}

TEST(tui_parts_with_none_says_so) {
    Snapshot s;
    Ctx ctx(30, 3);
    ardio::tui::draw_parts(ctx, hike_rect{0, 0, 30, 3}, s);
    CHECK(row_contains(ctx, 0, 30, "no parts wired"));
}

TEST(tui_parts_too_many_for_the_rows_says_how_many_are_hidden) {
    Snapshot s;
    for (int i = 0; i < 6; ++i) {
        s.parts.push_back(PartView{"led", i, "off"});
    }
    Ctx ctx(30, 3);
    ardio::tui::draw_parts(ctx, hike_rect{0, 0, 30, 3}, s);
    CHECK(row_contains(ctx, 2, 30, "+4 more"));
}

TEST(tui_parts_clipped_to_a_tiny_rect_writes_nothing_outside) {
    Snapshot s;
    for (int i = 0; i < 6; ++i) {
        s.parts.push_back(PartView{"servo", i, "a long description indeed"});
    }
    Ctx ctx(40, 10);
    fill_sentinel(ctx, 40, 10);
    hike_rect area{5, 4, 7, 2};
    ardio::tui::draw_parts(ctx, area, s);
    CHECK(untouched_outside(ctx, 40, 10, area));
}

// ------------------------------------------------------------------- cpu ---

TEST(tui_cpu_shows_pc_sp_and_registers) {
    Snapshot s;
    s.pc = 0x0123;
    s.sp = 0x08FE;
    s.r[0] = 0xAB;
    s.r[31] = 0x0F;

    Ctx ctx(60, 8);
    ardio::tui::draw_cpu(ctx, hike_rect{0, 0, 60, 8}, s);

    CHECK(row_contains(ctx, 0, 60, "PC 0123"));
    CHECK(row_contains(ctx, 0, 60, "SP 08FE"));
    CHECK(grid_contains(ctx, 60, 8, "R00 AB"));
    CHECK(grid_contains(ctx, 60, 8, "R31 0F"));
    CHECK(grid_contains(ctx, 60, 8, "R05 00"));
}

TEST(tui_cpu_sreg_flags_are_spelled_out_with_case_carrying_state) {
    Snapshot s;
    // I set (bit 7), Z set (bit 1); everything else clear.
    s.sreg = 0x82;

    Ctx ctx(40, 6);
    ardio::tui::draw_cpu(ctx, hike_rect{0, 0, 40, 6}, s);

    CHECK(row_contains(ctx, 1, 40, "SREG IthsvnZc"));
}

TEST(tui_cpu_sreg_all_set_and_all_clear) {
    Snapshot s;
    s.sreg = 0xFF;
    Ctx a(40, 6);
    ardio::tui::draw_cpu(a, hike_rect{0, 0, 40, 6}, s);
    CHECK(row_contains(a, 1, 40, "SREG ITHSVNZC"));

    s.sreg = 0x00;
    Ctx b(40, 6);
    ardio::tui::draw_cpu(b, hike_rect{0, 0, 40, 6}, s);
    CHECK(row_contains(b, 1, 40, "SREG ithsvnzc"));
}

TEST(tui_cpu_set_flags_are_bold_as_well_as_upper_case) {
    Snapshot s;
    s.sreg = 0x80;  // I only
    Ctx ctx(40, 6);
    ardio::tui::draw_cpu(ctx, hike_rect{0, 0, 40, 6}, s);
    // "SREG " is five columns, so the I is at column 5 and T at 6.
    CHECK((mono(ctx, 5, 1).attrs & HIKE_BOLD) != 0);
    CHECK((mono(ctx, 6, 1).attrs & HIKE_BOLD) == 0);
}

TEST(tui_cpu_narrow_keeps_pc_and_flags_and_drops_the_register_file) {
    Snapshot s;
    s.pc = 0x0010;
    s.sreg = 0x01;
    s.r[0] = 0x77;

    Ctx ctx(4, 6);
    fill_sentinel(ctx, 4, 6);
    ardio::tui::draw_cpu(ctx, hike_rect{0, 0, 4, 6}, s);

    CHECK((whole_row(ctx, 0, 4) == std::string("PC 0")));
    // Too narrow even for the "SREG" caption, so the flags take the row on
    // their own -- the letters are the data and the caption is decoration.
    CHECK((whole_row(ctx, 1, 4) == std::string("iths")));
    // A register cell is six columns and there is no room for one, so none is
    // drawn rather than half of one: a number cut in half still looks like a
    // number and would be read as one.
    CHECK(!grid_contains(ctx, 4, 6, "R00"));
}

TEST(tui_cpu_drops_the_sreg_caption_before_it_drops_a_flag) {
    Snapshot s;
    s.sreg = 0x02;  // Z only

    Ctx ctx(10, 4);
    ardio::tui::draw_cpu(ctx, hike_rect{0, 0, 10, 4}, s);
    // Ten columns cannot hold "SREG " and eight letters, so the caption goes
    // and every flag survives.
    CHECK((whole_row(ctx, 1, 8) == std::string("ithsvnZc")));
}

TEST(tui_cpu_one_row_high_shows_only_the_program_counter) {
    Snapshot s;
    s.pc = 0x0042;
    Ctx ctx(40, 6);
    fill_sentinel(ctx, 40, 6);
    hike_rect area{0, 0, 40, 1};
    ardio::tui::draw_cpu(ctx, area, s);
    CHECK(row_contains(ctx, 0, 40, "PC 0042"));
    CHECK(untouched_outside(ctx, 40, 6, area));
}

TEST(tui_cpu_clipped_to_a_tiny_rect_writes_nothing_outside) {
    Snapshot s;
    s.pc = 0x1234;
    s.sreg = 0xFF;
    for (int i = 0; i < 32; ++i) s.r[i] = uint8_t(i + 1);

    Ctx ctx(50, 14);
    fill_sentinel(ctx, 50, 14);
    hike_rect area{4, 3, 9, 4};
    ardio::tui::draw_cpu(ctx, area, s);
    CHECK(untouched_outside(ctx, 50, 14, area));
}

// ---------------------------------------------------------------- serial ---

TEST(tui_serial_shows_the_newest_lines_by_default) {
    Snapshot s;
    s.serial = "one\ntwo\nthree\nfour\n";
    Ctx ctx(10, 2);
    ardio::tui::draw_serial(ctx, hike_rect{0, 0, 10, 2}, s, 0);
    CHECK((whole_row(ctx, 0, 5) == std::string("three")));
    CHECK((whole_row(ctx, 1, 4) == std::string("four")));
}

TEST(tui_serial_scrolls_back_by_lines) {
    Snapshot s;
    s.serial = "one\ntwo\nthree\nfour\n";
    Ctx ctx(10, 2);
    ardio::tui::draw_serial(ctx, hike_rect{0, 0, 10, 2}, s, 1);
    CHECK((whole_row(ctx, 0, 3) == std::string("two")));
    CHECK((whole_row(ctx, 1, 5) == std::string("three")));
}

TEST(tui_serial_scroll_clamps_at_the_top) {
    Snapshot s;
    s.serial = "one\ntwo\nthree\nfour\n";
    Ctx ctx(10, 2);
    ardio::tui::draw_serial(ctx, hike_rect{0, 0, 10, 2}, s, 99);
    CHECK((whole_row(ctx, 0, 3) == std::string("one")));
    CHECK((whole_row(ctx, 1, 3) == std::string("two")));
}

TEST(tui_serial_negative_scroll_clamps_at_the_bottom) {
    Snapshot s;
    s.serial = "one\ntwo\nthree\nfour\n";
    Ctx ctx(10, 2);
    ardio::tui::draw_serial(ctx, hike_rect{0, 0, 10, 2}, s, -5);
    CHECK((whole_row(ctx, 0, 5) == std::string("three")));
    CHECK((whole_row(ctx, 1, 4) == std::string("four")));
}

TEST(tui_serial_with_fewer_lines_than_rows_starts_at_the_top) {
    Snapshot s;
    s.serial = "one\ntwo\n";
    Ctx ctx(10, 5);
    fill_sentinel(ctx, 10, 5);
    ardio::tui::draw_serial(ctx, hike_rect{0, 0, 10, 5}, s, 0);
    CHECK((whole_row(ctx, 0, 3) == std::string("one")));
    CHECK((whole_row(ctx, 1, 3) == std::string("two")));
    // The rows past the end are left alone rather than blanked, so a panel
    // drawn underneath is not erased by an empty log.
    CHECK((whole_row(ctx, 2, 3) == std::string("###")));
}

TEST(tui_serial_keeps_a_line_that_has_no_newline_yet) {
    Snapshot s;
    s.serial = "done\npartial";
    Ctx ctx(10, 3);
    ardio::tui::draw_serial(ctx, hike_rect{0, 0, 10, 3}, s, 0);
    CHECK(grid_contains(ctx, 10, 3, "partial"));
}

TEST(tui_serial_with_no_output_says_so) {
    Snapshot s;
    Ctx ctx(20, 3);
    ardio::tui::draw_serial(ctx, hike_rect{0, 0, 20, 3}, s, 0);
    CHECK(row_contains(ctx, 0, 20, "no output yet"));
}

TEST(tui_serial_clipped_to_a_tiny_rect_writes_nothing_outside) {
    Snapshot s;
    s.serial = "a very long line indeed that will not fit\nsecond\nthird\n";
    Ctx ctx(40, 8);
    fill_sentinel(ctx, 40, 8);
    hike_rect area{6, 2, 5, 2};
    ardio::tui::draw_serial(ctx, area, s, 0);
    CHECK(untouched_outside(ctx, 40, 8, area));
}

// ---------------------------------------------------------------- status ---

TEST(tui_status_shows_board_core_time_and_speed) {
    Snapshot s = basic_snapshot();
    Ctx ctx(70, 1);
    ardio::tui::draw_status(ctx, hike_rect{0, 0, 70, 1}, s);
    std::string row = whole_row(ctx, 0, 70);
    CHECK(row.find("RUNNING") == 0);
    CHECK(row.find("uno") != std::string::npos);
    CHECK(row.find("jit") != std::string::npos);
    CHECK(row.find("t=1.500s") != std::string::npos);
    CHECK(row.find("12.50 MIPS") != std::string::npos);
    CHECK(row.find("blink.ino") != std::string::npos);
}

TEST(tui_status_paused_halted_and_running_are_different_words) {
    Snapshot s = basic_snapshot();
    Ctx a(40, 1);
    s.running = false;
    ardio::tui::draw_status(a, hike_rect{0, 0, 40, 1}, s);
    CHECK(row_contains(a, 0, 40, "PAUSED"));

    Ctx b(40, 1);
    s.halted = true;
    ardio::tui::draw_status(b, hike_rect{0, 0, 40, 1}, s);
    CHECK(row_contains(b, 0, 40, "HALTED"));
}

TEST(tui_status_faulted_shows_the_fault_not_a_normal_run) {
    Snapshot s = basic_snapshot();
    s.fault = "bad opcode at 0x0100";

    Ctx ctx(60, 1);
    ardio::tui::draw_status(ctx, hike_rect{0, 0, 60, 1}, s);
    std::string row = whole_row(ctx, 0, 60);
    CHECK(row.find("FAULTED") == 0);
    CHECK(row.find("bad opcode at 0x0100") != std::string::npos);
    // The timing fields are displaced by the message rather than sharing the
    // line with it: a fault is the only thing worth reading here.
    CHECK(row.find("MIPS") == std::string::npos);
    CHECK(row.find("RUNNING") == std::string::npos);
    // Reversed as well as coloured, so it cannot look like a normal run on a
    // terminal with no colour.
    CHECK((mono(ctx, 0, 0).attrs & HIKE_REVERSE) != 0);
}

TEST(tui_status_not_faulted_is_not_reversed) {
    Snapshot s = basic_snapshot();
    Ctx ctx(60, 1);
    ardio::tui::draw_status(ctx, hike_rect{0, 0, 60, 1}, s);
    CHECK((mono(ctx, 0, 0).attrs & HIKE_REVERSE) == 0);
}

TEST(tui_status_narrow_keeps_the_state_word_first) {
    Snapshot s = basic_snapshot();
    Ctx ctx(4, 1);
    ardio::tui::draw_status(ctx, hike_rect{0, 0, 4, 1}, s);
    CHECK((whole_row(ctx, 0, 4) == std::string("RUNN")));
}

TEST(tui_status_clipped_to_a_tiny_rect_writes_nothing_outside) {
    Snapshot s = basic_snapshot();
    s.fault = "a fault message that is far too long for this rect";
    Ctx ctx(40, 5);
    fill_sentinel(ctx, 40, 5);
    hike_rect area{8, 3, 6, 1};
    ardio::tui::draw_status(ctx, area, s);
    CHECK(untouched_outside(ctx, 40, 5, area));
}

// ------------------------------------------------------- drawing is pure ---

TEST(tui_views_drawn_twice_give_the_same_grid) {
    // The snapshot is the whole input, so a second draw of the same snapshot
    // must produce the same cells. This is what makes a redraw free, and it
    // would fail if a view ever kept state of its own between frames.
    Snapshot s = basic_snapshot();
    s.serial = "hello\nworld\n";

    Ctx a(60, 20);
    Ctx b(60, 20);
    for (hike_context* c : {a.ctx, b.ctx}) {
        ardio::tui::draw_board(c, hike_rect{0, 0, 60, 8}, s);
        ardio::tui::draw_parts(c, hike_rect{0, 8, 60, 2}, s);
        ardio::tui::draw_cpu(c, hike_rect{0, 10, 60, 6}, s);
        ardio::tui::draw_serial(c, hike_rect{0, 16, 60, 3}, s, 0);
        ardio::tui::draw_status(c, hike_rect{0, 19, 60, 1}, s);
    }
    bool same = true;
    for (int y = 0; y < 20 && same; ++y) {
        for (int x = 0; x < 60; ++x) {
            hike_cell ca = hike_get_cell(a, x, y);
            hike_cell cb = hike_get_cell(b, x, y);
            if (ca.ch != cb.ch || ca.attrs != cb.attrs) { same = false; break; }
        }
    }
    CHECK(same);
    // And the same snapshot drawn again into the first context changes nothing.
    ardio::tui::draw_status(a, hike_rect{0, 19, 60, 1}, s);
    CHECK((mono_row(a, 19, 7) == std::string("RUNNING")));
}

TEST(tui_views_accept_a_zero_sized_rect) {
    Snapshot s = basic_snapshot();
    Ctx ctx(20, 4);
    fill_sentinel(ctx, 20, 4);
    hike_rect empty{2, 2, 0, 0};
    ardio::tui::draw_board(ctx, empty, s);
    ardio::tui::draw_parts(ctx, empty, s);
    ardio::tui::draw_cpu(ctx, empty, s);
    ardio::tui::draw_serial(ctx, empty, s, 0);
    ardio::tui::draw_status(ctx, empty, s);
    CHECK(untouched_outside(ctx, 20, 4, empty));
}
