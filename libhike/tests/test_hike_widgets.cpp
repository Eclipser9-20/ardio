// Tests for libhike's layout, widgets and C++ wrapper.
//
// Everything here asserts on CELL CONTENTS. A widget's job is to put
// particular characters in particular columns, so that is what is checked; a
// test that read the widget's own struct back would pass just as happily if
// nothing were ever drawn. The layout tests are the exception and assert on
// rects, because rects are what hike_layout_split returns and nothing is drawn
// at all -- but every rect assertion is arithmetic worked out by hand from the
// rules in widgets.h, never copied from a run.
//
// There is no terminal in CI, so these run on a memory context: the same grid,
// the same drawing code, an in-memory sink instead of a file descriptor.

#include "harness.h"

#include "hike/hike.hpp"
#include "hike/widgets.h"

#include "../src/hike_internal.h"

#include <memory>
#include <string>
#include <vector>

namespace {

// A memory context that frees itself, so a failing CHECK cannot leak one.
struct Ctx {
    hike_context* ctx;
    explicit Ctx(int w, int h) : ctx(hike_context_new_memory(w, h, HIKE_COLOR_TRUE)) {}
    ~Ctx() { hike_context_free(ctx); }
    Ctx(const Ctx&) = delete;
    Ctx& operator=(const Ctx&) = delete;
    operator hike_context*() const { return ctx; }
};

// The characters of one row, as a string. Code points outside ASCII become '?'
// so an assertion can be written as a readable literal; the box-drawing tests
// check those code points directly instead.
std::string row_text(hike_context* ctx, int y, int x, int w) {
    std::string out;
    for (int i = 0; i < w; ++i) {
        uint32_t ch = hike_get_cell(ctx, x + i, y).ch;
        out.push_back(ch >= 0x20 && ch < 0x7F ? char(ch) : '?');
    }
    return out;
}

uint32_t cell_char(hike_context* ctx, int x, int y) {
    return hike_get_cell(ctx, x, y).ch;
}

hike_event key_event(hike_key key, uint32_t ch = 0, uint8_t mods = 0) {
    hike_event ev{};
    ev.kind = HIKE_EVENT_KEY;
    ev.key.key = key;
    ev.key.ch = ch;
    ev.key.mods = mods;
    return ev;
}

hike_event char_event(char c) { return key_event(HIKE_KEY_CHAR, uint32_t(c)); }

bool grids_equal(hike_context* a, hike_context* b, int w, int h) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            hike_cell ca = hike_get_cell(a, x, y);
            hike_cell cb = hike_get_cell(b, x, y);
            if (ca.ch != cb.ch || ca.attrs != cb.attrs) return false;
            if (ca.fg.kind != cb.fg.kind || ca.bg.kind != cb.bg.kind) return false;
        }
    }
    return true;
}

} // namespace

// --------------------------------------------------------------- layout ---

TEST(hike_layout_row_fixed_content_and_weight) {
    // 30 columns, no padding, no gaps. fixed 6 and content 4 take 10, leaving
    // 20 for the two weights in a 3:1 ratio -- 15 and 5.
    const hike_size sizes[] = {hike_fixed(6), hike_weight(3), hike_content(4), hike_weight(1)};
    hike_rect out[4];
    CHECK_EQ(hike_layout_split(hike_row(), (hike_rect){0, 0, 30, 5}, sizes, 4, out), 4);

    CHECK_EQ(out[0].x, 0);  CHECK_EQ(out[0].w, 6);
    CHECK_EQ(out[1].x, 6);  CHECK_EQ(out[1].w, 15);
    CHECK_EQ(out[2].x, 21); CHECK_EQ(out[2].w, 4);
    CHECK_EQ(out[3].x, 25); CHECK_EQ(out[3].w, 5);
    // The cross axis is the full height for every child in a row.
    for (int i = 0; i < 4; ++i) { CHECK_EQ(out[i].y, 0); CHECK_EQ(out[i].h, 5); }
}

TEST(hike_layout_rounding_gives_the_extra_cells_to_the_left) {
    // 10 columns across three equal weights is 3.33 each. Floor gives 3, 3, 3
    // and one cell is left over, which goes to the first child: 4, 3, 3. The
    // widths must sum to exactly the space available, with no gap at the end.
    const hike_size sizes[] = {hike_weight(1), hike_weight(1), hike_weight(1)};
    hike_rect out[3];
    hike_layout_split(hike_row(), (hike_rect){0, 0, 10, 1}, sizes, 3, out);
    CHECK_EQ(out[0].w, 4);
    CHECK_EQ(out[1].w, 3);
    CHECK_EQ(out[2].w, 3);
    CHECK_EQ(out[0].w + out[1].w + out[2].w, 10);
    CHECK_EQ(out[1].x, 4);
    CHECK_EQ(out[2].x, 7);
}

TEST(hike_layout_rounding_with_uneven_weights) {
    // 11 columns, weights 1 and 2: floor(11/3)=3 and floor(22/3)=7, which is
    // 10, so the odd cell goes to the earlier weighted child.
    const hike_size sizes[] = {hike_weight(1), hike_weight(2)};
    hike_rect out[2];
    hike_layout_split(hike_row(), (hike_rect){0, 0, 11, 1}, sizes, 2, out);
    CHECK_EQ(out[0].w, 4);
    CHECK_EQ(out[1].w, 7);
}

TEST(hike_layout_padding_and_gaps_come_off_first) {
    // 20 wide, 1 of padding each side leaves 18; two gaps of 2 leave 14 for
    // three equal weights. 14 does not divide by 3: the floor is 4 each and
    // the two spare cells go to the first two children, giving 5, 5, 4.
    hike_layout l = hike_layout_pad(hike_row(), 1);
    l.gap = 2;
    const hike_size sizes[] = {hike_weight(1), hike_weight(1), hike_weight(1)};
    hike_rect out[3];
    hike_layout_split(l, (hike_rect){0, 0, 20, 6}, sizes, 3, out);
    CHECK_EQ(out[0].x, 1);
    CHECK_EQ(out[0].w, 5);
    CHECK_EQ(out[1].x, 8);
    CHECK_EQ(out[1].w, 5);
    CHECK_EQ(out[2].x, 15);
    CHECK_EQ(out[2].w, 4);
    CHECK_EQ(out[0].y, 1);
    CHECK_EQ(out[0].h, 4);
}

TEST(hike_layout_starves_the_last_child_when_space_runs_out) {
    // 10 columns, three fixed children asking for 6, 6 and 6. The first is
    // served in full, the second gets what is left, the third gets nothing --
    // and still has a position, so indices do not shift.
    const hike_size sizes[] = {hike_fixed(6), hike_fixed(6), hike_fixed(6)};
    hike_rect out[3];
    hike_layout_split(hike_row(), (hike_rect){0, 0, 10, 1}, sizes, 3, out);
    CHECK_EQ(out[0].w, 6);
    CHECK_EQ(out[1].w, 4);
    CHECK_EQ(out[2].w, 0);
    CHECK_EQ(out[2].x, 10);
}

TEST(hike_layout_leftover_is_not_given_away_without_a_weight) {
    const hike_size sizes[] = {hike_fixed(3), hike_fixed(3)};
    hike_rect out[2];
    hike_layout_split(hike_row(), (hike_rect){0, 0, 20, 1}, sizes, 2, out);
    CHECK_EQ(out[0].w, 3);
    CHECK_EQ(out[1].w, 3);   // not stretched: fixed means fixed
}

TEST(hike_layout_column_splits_the_height) {
    const hike_size sizes[] = {hike_fixed(2), hike_weight(1)};
    hike_rect out[2];
    hike_layout_split(hike_column(), (hike_rect){4, 3, 8, 10}, sizes, 2, out);
    CHECK_EQ(out[0].y, 3);  CHECK_EQ(out[0].h, 2);  CHECK_EQ(out[0].w, 8);
    CHECK_EQ(out[1].y, 5);  CHECK_EQ(out[1].h, 8);  CHECK_EQ(out[1].x, 4);
}

TEST(hike_layout_nests) {
    // A column of two rows, the second split again. The inner split sees the
    // outer child's rect and nothing else, which is what makes nesting work
    // without a tree.
    const hike_size outer[] = {hike_fixed(1), hike_weight(1)};
    hike_rect rows[2];
    hike_layout_split(hike_column(), (hike_rect){0, 0, 12, 5}, outer, 2, rows);
    CHECK_EQ(rows[1].y, 1);
    CHECK_EQ(rows[1].h, 4);

    const hike_size inner[] = {hike_weight(1), hike_weight(1)};
    hike_rect cols[2];
    hike_layout_split(hike_row(), rows[1], inner, 2, cols);
    CHECK_EQ(cols[0].x, 0);  CHECK_EQ(cols[0].w, 6);
    CHECK_EQ(cols[1].x, 6);  CHECK_EQ(cols[1].w, 6);
    CHECK_EQ(cols[0].y, 1);  CHECK_EQ(cols[0].h, 4);
}

// ---------------------------------------------------------------- label ---

TEST(hike_label_aligns_and_clips) {
    Ctx ctx(12, 3);
    hike_label(ctx, (hike_rect){0, 0, 12, 1}, "hi", HIKE_ALIGN_RIGHT, hike_style_default());
    CHECK(row_text(ctx, 0, 10, 2) == "hi");

    hike_label(ctx, (hike_rect){0, 1, 5, 1}, "abcdefghij", HIKE_ALIGN_LEFT, hike_style_default());
    CHECK(row_text(ctx, 1, 0, 6) == "abcde ");   // the sixth cell was never written
}

TEST(hike_label_clipped_by_a_container_smaller_than_it) {
    // The container pushes a clip and the label draws past it. Nothing outside
    // the clip may change, which is the guarantee a container relies on.
    Ctx ctx(20, 2);
    hike_fill(ctx, (hike_rect){0, 0, 20, 2}, (hike_cell){'.', hike_default_color(), hike_default_color(), 0});
    hike_push_clip(ctx, (hike_rect){2, 0, 4, 1});
    hike_label(ctx, (hike_rect){2, 0, 16, 1}, "abcdefghijkl", HIKE_ALIGN_LEFT, hike_style_default());
    hike_pop_clip(ctx);
    CHECK(row_text(ctx, 0, 0, 10) == "..abcd....");
}

// ------------------------------------------------------------------ box ---

TEST(hike_box_draws_the_right_corners) {
    Ctx ctx(8, 4);
    hike_rect inner = hike_box(ctx, (hike_rect){0, 0, 6, 3}, HIKE_BORDER_SINGLE, nullptr,
                               hike_style_default());
    CHECK_EQ(inner.x, 1);
    CHECK_EQ(inner.y, 1);
    CHECK_EQ(inner.w, 4);
    CHECK_EQ(inner.h, 1);

    CHECK_EQ(int(cell_char(ctx, 0, 0)), 0x250C);   // top left
    CHECK_EQ(int(cell_char(ctx, 5, 0)), 0x2510);   // top right
    CHECK_EQ(int(cell_char(ctx, 0, 2)), 0x2514);   // bottom left
    CHECK_EQ(int(cell_char(ctx, 5, 2)), 0x2518);   // bottom right
    CHECK_EQ(int(cell_char(ctx, 2, 0)), 0x2500);   // horizontal
    CHECK_EQ(int(cell_char(ctx, 0, 1)), 0x2502);   // vertical
}

TEST(hike_box_rounded_and_ascii_use_their_own_corners) {
    Ctx ctx(8, 4);
    hike_box(ctx, (hike_rect){0, 0, 4, 3}, HIKE_BORDER_ROUNDED, nullptr, hike_style_default());
    CHECK_EQ(int(cell_char(ctx, 0, 0)), 0x256D);
    CHECK_EQ(int(cell_char(ctx, 3, 2)), 0x256F);

    hike_box(ctx, (hike_rect){4, 0, 4, 3}, HIKE_BORDER_ASCII, nullptr, hike_style_default());
    CHECK_EQ(int(cell_char(ctx, 4, 0)), '+');
    CHECK_EQ(int(cell_char(ctx, 5, 0)), '-');
    CHECK_EQ(int(cell_char(ctx, 4, 1)), '|');
}

TEST(hike_box_title_sits_in_the_top_edge_with_padding) {
    Ctx ctx(14, 3);
    hike_box(ctx, (hike_rect){0, 0, 12, 3}, HIKE_BORDER_ASCII, "Log", hike_style_default());
    CHECK(row_text(ctx, 0, 0, 12) == "+ Log -----+");
}

TEST(hike_box_drops_a_title_that_does_not_fit) {
    // Six columns cannot hold "Settings" with its padding, so the top edge
    // stays a plain border rather than showing a cut-off word.
    Ctx ctx(8, 3);
    hike_box(ctx, (hike_rect){0, 0, 6, 3}, HIKE_BORDER_ASCII, "Settings", hike_style_default());
    CHECK(row_text(ctx, 0, 0, 6) == "+----+");
}

// --------------------------------------------------------------- button ---

TEST(hike_button_draws_brackets_and_activates) {
    Ctx ctx(12, 1);
    hike_button b = hike_button_make("Save");
    b.focused = true;
    hike_button_draw(ctx, (hike_rect){0, 0, 12, 1}, &b);
    // "[ Save ]" is eight columns centred in twelve, so it starts at column 2.
    CHECK(row_text(ctx, 0, 0, 12) == "  [ Save ]  ");

    hike_event enter = key_event(HIKE_KEY_ENTER);
    CHECK(hike_button_event(&b, (hike_rect){0, 0, 12, 1}, &enter));
    hike_event space = char_event(' ');
    CHECK(hike_button_event(&b, (hike_rect){0, 0, 12, 1}, &space));

    // The focus rule: an unfocused button sees no key at all.
    b.focused = false;
    CHECK(!hike_button_event(&b, (hike_rect){0, 0, 12, 1}, &enter));
}

TEST(hike_button_is_clicked_by_position_not_focus) {
    hike_button b = hike_button_make("Ok");
    hike_event ev{};
    ev.kind = HIKE_EVENT_MOUSE;
    ev.mouse.kind = HIKE_MOUSE_PRESS;
    ev.mouse.x = 3;
    ev.mouse.y = 0;
    CHECK(hike_button_event(&b, (hike_rect){0, 0, 8, 1}, &ev));
    ev.mouse.x = 30;
    CHECK(!hike_button_event(&b, (hike_rect){0, 0, 8, 1}, &ev));
}

// ----------------------------------------------------------- text input ---

TEST(hike_input_scrolls_to_keep_the_cursor_visible) {
    Ctx ctx(20, 1);
    char buf[64];
    hike_input in = hike_input_make(buf, sizeof buf);
    in.focused = true;
    hike_input_set_text(&in, "abcdefghij");     // ten characters
    const hike_rect r{0, 0, 6, 1};              // a six column field

    // The cursor sits after the last character, at column 10. The rightmost
    // usable column is 5, so the view scrolls by 10 - 5 = 5 and shows f..j
    // with the cursor in the free column at the end.
    hike_input_draw(ctx, r, &in);
    CHECK(row_text(ctx, 0, 0, 6) == "fghij ");
    CHECK_EQ(in.scroll, 5);
    CHECK_EQ(hike_input_cursor_column(&in), 5);

    // Home scrolls all the way back.
    hike_event home = key_event(HIKE_KEY_HOME);
    CHECK(hike_input_event(&in, r.w, &home));
    hike_input_draw(ctx, r, &in);
    CHECK(row_text(ctx, 0, 0, 6) == "abcdef");
    CHECK_EQ(hike_input_cursor_column(&in), 0);

    // Walking right past the edge scrolls by exactly one column at a time.
    for (int i = 0; i < 6; ++i) {
        hike_event right = key_event(HIKE_KEY_RIGHT);
        hike_input_event(&in, r.w, &right);
    }
    hike_input_draw(ctx, r, &in);
    CHECK_EQ(in.scroll, 1);
    CHECK(row_text(ctx, 0, 0, 6) == "bcdefg");
    CHECK_EQ(hike_input_cursor_column(&in), 5);
}

TEST(hike_input_typing_and_erasing_show_in_the_cells) {
    Ctx ctx(10, 1);
    char buf[16];
    hike_input in = hike_input_make(buf, sizeof buf);
    in.focused = true;
    for (const char* p = "hey"; *p; ++p) {
        hike_event ev = char_event(*p);
        CHECK(hike_input_event(&in, 10, &ev));
    }
    hike_input_draw(ctx, (hike_rect){0, 0, 10, 1}, &in);
    CHECK(row_text(ctx, 0, 0, 5) == "hey  ");

    hike_event back = key_event(HIKE_KEY_BACKSPACE);
    hike_input_event(&in, 10, &back);
    hike_input_draw(ctx, (hike_rect){0, 0, 10, 1}, &in);
    CHECK(row_text(ctx, 0, 0, 5) == "he   ");

    // An unfocused input takes nothing, by the focus rule.
    in.focused = false;
    hike_event ev = char_event('x');
    CHECK(!hike_input_event(&in, 10, &ev));
}

TEST(hike_input_masked_draws_asterisks_not_text) {
    Ctx ctx(10, 1);
    char buf[16];
    hike_input in = hike_input_make(buf, sizeof buf);
    in.masked = true;
    hike_input_set_text(&in, "hunter2");
    hike_input_draw(ctx, (hike_rect){0, 0, 10, 1}, &in);
    CHECK(row_text(ctx, 0, 0, 8) == "******* ");
}

TEST(hike_input_refuses_to_overflow_its_buffer) {
    char buf[4];   // three characters plus a terminator
    hike_input in = hike_input_make(buf, sizeof buf);
    in.focused = true;
    for (int i = 0; i < 6; ++i) {
        hike_event ev = char_event('a');
        hike_input_event(&in, 10, &ev);
    }
    CHECK_EQ(int(in.len), 3);
    CHECK(std::string(buf) == "aaa");
}

// ----------------------------------------------------------------- list ---

TEST(hike_list_selection_and_scrolling_at_both_ends) {
    Ctx ctx(6, 3);
    const char* const items[] = {"one", "two", "three", "four", "five"};
    hike_list l = hike_list_make(items, 5);
    l.focused = true;
    const hike_rect r{0, 0, 6, 3};

    hike_list_draw(ctx, r, &l);
    CHECK(row_text(ctx, 0, 0, 6) == "one   ");
    CHECK(row_text(ctx, 2, 0, 6) == "three ");

    // Down three times: the first two move within the view, the third pushes
    // the view down by one so the selection stays on screen.
    for (int i = 0; i < 3; ++i) {
        hike_event down = key_event(HIKE_KEY_DOWN);
        CHECK(hike_list_event(&l, r.h, &down));
    }
    CHECK_EQ(l.selected, 3);
    CHECK_EQ(l.scroll, 1);
    hike_list_draw(ctx, r, &l);
    CHECK(row_text(ctx, 0, 0, 6) == "two   ");
    CHECK(row_text(ctx, 2, 0, 6) == "four  ");

    // At the bottom the selection stops rather than wrapping, and the extra
    // key is reported as having changed nothing.
    hike_event down = key_event(HIKE_KEY_DOWN);
    CHECK(hike_list_event(&l, r.h, &down));
    CHECK(!hike_list_event(&l, r.h, &down));
    CHECK_EQ(l.selected, 4);
    CHECK_EQ(l.scroll, 2);
    hike_list_draw(ctx, r, &l);
    CHECK(row_text(ctx, 2, 0, 6) == "five  ");

    // Home returns to the top and scrolls back with it.
    hike_event home = key_event(HIKE_KEY_HOME);
    CHECK(hike_list_event(&l, r.h, &home));
    hike_list_draw(ctx, r, &l);
    CHECK_EQ(l.scroll, 0);
    CHECK(row_text(ctx, 0, 0, 6) == "one   ");
    hike_event up = key_event(HIKE_KEY_UP);
    CHECK(!hike_list_event(&l, r.h, &up));    // already at the top
}

TEST(hike_list_shorter_than_its_rect_leaves_blank_rows) {
    Ctx ctx(6, 4);
    const char* const items[] = {"a", "b"};
    hike_list l = hike_list_make(items, 2);
    hike_list_draw(ctx, (hike_rect){0, 0, 6, 4}, &l);
    CHECK(row_text(ctx, 2, 0, 6) == "      ");
    CHECK(row_text(ctx, 3, 0, 6) == "      ");
    CHECK_EQ(l.scroll, 0);
}

TEST(hike_list_marks_the_selected_row_with_reverse_video) {
    // The selection has to be visible on a terminal with no colour, so the
    // default theme reverses it. This is a cell attribute, not a character.
    Ctx ctx(6, 2);
    const char* const items[] = {"a", "b"};
    hike_list l = hike_list_make(items, 2);
    l.selected = 1;
    hike_list_draw(ctx, (hike_rect){0, 0, 6, 2}, &l);
    CHECK((hike_get_cell(ctx, 0, 1).attrs & HIKE_REVERSE) != 0);
    CHECK((hike_get_cell(ctx, 0, 0).attrs & HIKE_REVERSE) == 0);
}

// ---------------------------------------------------- checkbox and radio ---

TEST(hike_checkbox_shows_and_toggles_its_state) {
    Ctx ctx(12, 1);
    hike_checkbox c = hike_checkbox_make("Wrap", false);
    c.focused = true;
    hike_checkbox_draw(ctx, (hike_rect){0, 0, 12, 1}, &c);
    CHECK(row_text(ctx, 0, 0, 10) == "[ ] Wrap  ");

    hike_event space = char_event(' ');
    CHECK(hike_checkbox_event(&c, (hike_rect){0, 0, 12, 1}, &space));
    hike_checkbox_draw(ctx, (hike_rect){0, 0, 12, 1}, &c);
    CHECK(row_text(ctx, 0, 0, 10) == "[x] Wrap  ");
}

TEST(hike_radio_moves_the_bullet_without_wrapping) {
    Ctx ctx(12, 3);
    const char* const labels[] = {"Low", "Mid", "High"};
    hike_radio_group g = hike_radio_make(labels, 3, 0);
    g.focused = true;
    hike_radio_draw(ctx, (hike_rect){0, 0, 12, 3}, &g);
    CHECK_EQ(int(cell_char(ctx, 1, 0)), 0x2022);
    CHECK_EQ(int(cell_char(ctx, 1, 1)), ' ');

    hike_event down = key_event(HIKE_KEY_DOWN);
    CHECK(hike_radio_event(&g, (hike_rect){0, 0, 12, 3}, &down));
    hike_radio_draw(ctx, (hike_rect){0, 0, 12, 3}, &g);
    CHECK_EQ(int(cell_char(ctx, 1, 0)), ' ');
    CHECK_EQ(int(cell_char(ctx, 1, 1)), 0x2022);

    hike_event up = key_event(HIKE_KEY_UP);
    hike_radio_event(&g, (hike_rect){0, 0, 12, 3}, &up);
    CHECK(!hike_radio_event(&g, (hike_rect){0, 0, 12, 3}, &up));   // stops at the top
}

// ------------------------------------------------------------- progress ---

TEST(hike_progress_never_reads_full_before_it_is) {
    Ctx ctx(10, 3);
    hike_progress p = hike_progress_make(0.5);
    hike_progress_draw(ctx, (hike_rect){0, 0, 10, 1}, &p);
    for (int i = 0; i < 5; ++i) CHECK_EQ(int(cell_char(ctx, i, 0)), 0x2588);
    for (int i = 5; i < 10; ++i) CHECK_EQ(int(cell_char(ctx, i, 0)), 0x2591);

    // 0.999 rounds to ten cells but must not draw a full bar.
    p.value = 0.999;
    hike_progress_draw(ctx, (hike_rect){0, 1, 10, 1}, &p);
    CHECK_EQ(int(cell_char(ctx, 9, 1)), 0x2591);

    p.value = 1.0;
    hike_progress_draw(ctx, (hike_rect){0, 2, 10, 1}, &p);
    CHECK_EQ(int(cell_char(ctx, 9, 2)), 0x2588);
}

// ------------------------------------------------------------- textview ---

TEST(hike_textview_scrolls_in_both_directions) {
    Ctx ctx(6, 2);
    const char* const lines[] = {"alpha", "bravo", "charlie"};
    hike_textview v = hike_textview_make(lines, 3);
    v.focused = true;
    const hike_rect r{0, 0, 6, 2};

    hike_textview_draw(ctx, r, &v);
    CHECK(row_text(ctx, 0, 0, 6) == "alpha ");

    hike_event down = key_event(HIKE_KEY_DOWN);
    CHECK(hike_textview_event(&v, r.h, &down));
    hike_textview_draw(ctx, r, &v);
    CHECK(row_text(ctx, 0, 0, 6) == "bravo ");
    CHECK(row_text(ctx, 1, 0, 6) == "charli");   // clipped, not wrapped

    hike_event right = key_event(HIKE_KEY_RIGHT);
    CHECK(hike_textview_event(&v, r.h, &right));
    CHECK(hike_textview_event(&v, r.h, &right));
    hike_textview_draw(ctx, r, &v);
    CHECK(row_text(ctx, 1, 0, 6) == "arlie ");
}

// ----------------------------------------------------------------- tabs ---

TEST(hike_tabs_draw_whole_and_hit_test_where_they_drew) {
    Ctx ctx(20, 1);
    const char* const labels[] = {"One", "Two"};
    hike_tabs t = hike_tabs_make(labels, 2);
    hike_tabs_draw(ctx, (hike_rect){0, 0, 20, 1}, &t);
    CHECK(row_text(ctx, 0, 0, 12) == " One  Two   ");

    hike_event ev{};
    ev.kind = HIKE_EVENT_MOUSE;
    ev.mouse.kind = HIKE_MOUSE_PRESS;
    ev.mouse.x = 6;    // inside the second tab, which spans columns 5..9
    ev.mouse.y = 0;
    CHECK(hike_tabs_event(&t, (hike_rect){0, 0, 20, 1}, &ev));
    CHECK_EQ(t.selected, 1);
}

// ---------------------------------------------------------------- focus ---

TEST(hike_focus_tab_traverses_and_wraps_both_ways) {
    hike_focus f = hike_focus_make(3);
    hike_event tab = key_event(HIKE_KEY_TAB);
    hike_event shift_tab = key_event(HIKE_KEY_TAB, 0, HIKE_MOD_SHIFT);

    CHECK_EQ(f.index, 0);
    CHECK(hike_focus_key(&f, &tab));      CHECK_EQ(f.index, 1);
    CHECK(hike_focus_key(&f, &tab));      CHECK_EQ(f.index, 2);
    CHECK(hike_focus_key(&f, &tab));      CHECK_EQ(f.index, 0);   // wraps forward
    CHECK(hike_focus_key(&f, &shift_tab)); CHECK_EQ(f.index, 2);  // wraps backward
    CHECK(hike_focus_key(&f, &shift_tab)); CHECK_EQ(f.index, 1);

    CHECK(hike_focus_has(&f, 1));
    CHECK(!hike_focus_has(&f, 0));

    // The ring consumes Tab and nothing else: everything else is the focused
    // widget's, which is the whole of the routing rule.
    hike_event down = key_event(HIKE_KEY_DOWN);
    CHECK(!hike_focus_key(&f, &down));
    CHECK_EQ(f.index, 1);
}

TEST(hike_focus_ring_routes_keys_to_exactly_one_widget) {
    // Two inputs, one focus ring. A character must land in the focused input
    // and in no other -- asserted on the cells, since that is where a user
    // would see the mistake.
    Ctx ctx(10, 2);
    char a[16], b[16];
    hike_input first = hike_input_make(a, sizeof a);
    hike_input second = hike_input_make(b, sizeof b);
    hike_focus ring = hike_focus_make(2);

    hike_event tab = key_event(HIKE_KEY_TAB);
    hike_focus_key(&ring, &tab);
    first.focused = hike_focus_has(&ring, 0);
    second.focused = hike_focus_has(&ring, 1);

    hike_event ev = char_event('z');
    CHECK(!hike_focus_key(&ring, &ev));
    hike_input_event(&first, 10, &ev);
    hike_input_event(&second, 10, &ev);

    hike_input_draw(ctx, (hike_rect){0, 0, 10, 1}, &first);
    hike_input_draw(ctx, (hike_rect){0, 1, 10, 1}, &second);
    CHECK(row_text(ctx, 0, 0, 2) == "  ");
    CHECK(row_text(ctx, 1, 0, 2) == "z ");
}

// ------------------------------------------------------------------ C++ ---
//
// The wrapper is faithful only if it is indistinguishable at the cells, so the
// tree tests below draw the same screen twice -- once by hand through the C
// API, once through hike::row -- and compare every cell. Everything else here
// tests what the tree adds over C: ownership by value, focus numbered by
// traversal order, and events routed to the widget the layout put under the
// pointer.

TEST(hike_cpp_tree_draws_the_same_cells_as_the_c_calls) {
    Ctx c_ctx(24, 3);
    Ctx cpp_ctx(24, 3);

    // By hand in C: split, then draw each widget into its rect.
    {
        const hike_size sizes[] = {hike_fixed(12), hike_weight(1)};
        hike_rect rects[2];
        hike_layout l = hike_row();
        l.gap = 1;
        hike_layout_split(l, (hike_rect){0, 0, 24, 3}, sizes, 2, rects);

        hike_button b = hike_button_make("Save");
        b.focused = true;
        hike_button_draw(c_ctx, rects[0], &b);
        hike_label(c_ctx, rects[1], "status", HIKE_ALIGN_LEFT, hike_style_default());
    }

    // The same screen as a tree. The layout, the rects and the draws are the
    // same C calls in the same order; only the way it is written differs.
    {
        hike::Context ctx = hike::Context::adopt(cpp_ctx.ctx);
        cpp_ctx.ctx = nullptr;

        auto ui = hike::row(
            hike::fixed(12, hike::button("Save").focused()),
            hike::weight(1, hike::label("status"))
        ).gap(1);
        ui.draw(ctx, hike::Rect{0, 0, 24, 3});

        CHECK(grids_equal(c_ctx, ctx.raw(), 24, 3));
        CHECK(row_text(ctx.raw(), 0, 0, 24) == "  [ Save ]   status     ");
    }
}

TEST(hike_cpp_tree_owns_its_children_by_value) {
    // Every child here is a temporary that is dead by the time draw runs. If
    // the tree held references this would be reading freed memory, so the test
    // is that the text still appears.
    Ctx raw(20, 2);
    hike::Context ctx = hike::Context::adopt(raw.ctx);
    raw.ctx = nullptr;

    hike::Layout ui = hike::column(
        hike::fixed(1, hike::label(std::string("first"))),
        hike::fixed(1, hike::label(std::string("second")))
    );
    ui.draw(ctx, hike::Rect{0, 0, 20, 2});
    CHECK(row_text(ctx.raw(), 0, 0, 6) == "first ");
    CHECK(row_text(ctx.raw(), 1, 0, 7) == "second ");
}

TEST(hike_cpp_tree_nests_and_a_container_clips_its_children) {
    // A row inside a column, and a label wider than the cell it was given. The
    // container pushes the C clip, so the label is cut at its own edge instead
    // of running into its neighbour.
    Ctx raw(12, 2);
    hike::Context ctx = hike::Context::adopt(raw.ctx);
    raw.ctx = nullptr;

    auto ui = hike::column(
        hike::fixed(1, hike::row(
            hike::fixed(4, hike::label("abcdefghij")),
            hike::weight(1, hike::label("XY"))
        )),
        hike::weight(1, hike::label("bottom"))
    );
    ui.draw(ctx, hike::Rect{0, 0, 12, 2});
    CHECK(row_text(ctx.raw(), 0, 0, 12) == "abcdXY      ");
    CHECK(row_text(ctx.raw(), 1, 0, 6) == "bottom");
}

TEST(hike_cpp_tree_uses_the_c_rounding_unchanged) {
    // Three equal weights across ten columns is 4, 3, 3 in C, and the tree
    // must not have opinions of its own about that.
    Ctx raw(10, 1);
    hike::Context ctx = hike::Context::adopt(raw.ctx);
    raw.ctx = nullptr;

    auto ui = hike::row(hike::label("a"), hike::label("b"), hike::label("c"));
    ui.draw(ctx, hike::Rect{0, 0, 10, 1});
    CHECK_EQ(ui.child_rect(0).w, 4);
    CHECK_EQ(ui.child_rect(1).w, 3);
    CHECK_EQ(ui.child_rect(2).w, 3);
    CHECK(row_text(ctx.raw(), 0, 0, 10) == "a   b  c  ");
}

TEST(hike_cpp_content_child_measures_the_widget) {
    // A content-sized child asks the widget how wide it wants to be, so the
    // number in the layout cannot drift from the text in the widget.
    Ctx raw(20, 1);
    hike::Context ctx = hike::Context::adopt(raw.ctx);
    raw.ctx = nullptr;

    auto ui = hike::row(
        hike::content(hike::button("Ok")),      // "[ Ok ]" is six columns
        hike::weight(1, hike::label("rest"))
    );
    ui.draw(ctx, hike::Rect{0, 0, 20, 1});
    CHECK_EQ(ui.child_rect(0).w, 6);
    CHECK_EQ(ui.child_rect(1).x, 6);
    CHECK(row_text(ctx.raw(), 0, 0, 12) == "[ Ok ]rest  ");
}

TEST(hike_cpp_box_draws_a_child_inside_its_frame) {
    Ctx raw(10, 4);
    hike::Context ctx = hike::Context::adopt(raw.ctx);
    raw.ctx = nullptr;

    auto ui = hike::box("T").ascii().child(hike::label("hi"));
    ui.draw(ctx, hike::Rect{0, 0, 8, 3});
    CHECK(row_text(ctx.raw(), 0, 0, 8) == "+ T ---+");
    CHECK(row_text(ctx.raw(), 1, 0, 8) == "|hi    |");
}

TEST(hike_cpp_focus_is_numbered_by_traversal_order) {
    // Three focusable widgets in two nested containers, and one label that is
    // not focusable and must not take a number. The assertion is on the cells:
    // the focused button is the bold one, and typing reaches one input only.
    Ctx raw(30, 3);
    hike::Context ctx = hike::Context::adopt(raw.ctx);
    raw.ctx = nullptr;

    hike::Focus focus;
    auto ui = hike::column(
        hike::fixed(1, hike::label("title")),
        hike::fixed(1, hike::row(
            hike::weight(1, hike::Input(32)),
            hike::weight(1, hike::Input(32))
        )),
        hike::fixed(1, hike::button("Go"))
    );

    ui.draw(ctx, hike::Rect{0, 0, 30, 3}, focus);
    CHECK_EQ(focus.count(), 3);      // the label is not in the ring
    CHECK_EQ(focus.index(), 0);

    // Typing goes to the first input and to nothing else.
    hike::Event z = char_event('z');
    CHECK(ui.dispatch(z, focus));
    ui.draw(ctx, hike::Rect{0, 0, 30, 3}, focus);
    CHECK(row_text(ctx.raw(), 1, 0, 2) == "z ");
    CHECK(row_text(ctx.raw(), 1, 15, 2) == "  ");

    // Tab moves to the second input, and the next character lands there.
    hike::Event tab = key_event(HIKE_KEY_TAB);
    CHECK(ui.dispatch(tab, focus));
    CHECK_EQ(focus.index(), 1);
    hike::Event q = char_event('q');
    CHECK(ui.dispatch(q, focus));
    ui.draw(ctx, hike::Rect{0, 0, 30, 3}, focus);
    CHECK(row_text(ctx.raw(), 1, 0, 2) == "z ");
    CHECK(row_text(ctx.raw(), 1, 15, 2) == "q ");

    // Tab again reaches the button, which is third in traversal order.
    CHECK(ui.dispatch(tab, focus));
    CHECK_EQ(focus.index(), 2);
    hike::Event enter = key_event(HIKE_KEY_ENTER);
    CHECK(ui.dispatch(enter, focus));

    // And Shift+Tab wraps back round the other way.
    hike::Event shift_tab = key_event(HIKE_KEY_TAB, 0, HIKE_MOD_SHIFT);
    CHECK(ui.dispatch(shift_tab, focus));
    CHECK_EQ(focus.index(), 1);
    for (int i = 0; i < 2; ++i) CHECK(ui.dispatch(shift_tab, focus));
    CHECK_EQ(focus.index(), 2);      // wrapped past the start
}

TEST(hike_cpp_a_key_reaches_the_focused_widget_and_no_other) {
    // The same rule as the C test, now with the tree doing the routing: only
    // one of two buttons may fire, whichever the ring points at.
    int first = 0, second = 0;
    hike::Focus focus;
    auto ui = hike::row(
        hike::weight(1, hike::button("A").on_click([&] { ++first; })),
        hike::weight(1, hike::button("B").on_click([&] { ++second; }))
    );

    Ctx raw(20, 1);
    hike::Context ctx = hike::Context::adopt(raw.ctx);
    raw.ctx = nullptr;
    ui.draw(ctx, hike::Rect{0, 0, 20, 1}, focus);

    hike::Event enter = key_event(HIKE_KEY_ENTER);
    CHECK(ui.dispatch(enter, focus));
    CHECK_EQ(first, 1);
    CHECK_EQ(second, 0);

    hike::Event tab = key_event(HIKE_KEY_TAB);
    ui.dispatch(tab, focus);
    ui.draw(ctx, hike::Rect{0, 0, 20, 1}, focus);
    CHECK(ui.dispatch(enter, focus));
    CHECK_EQ(first, 1);
    CHECK_EQ(second, 1);
}

TEST(hike_cpp_a_click_goes_where_the_pointer_is_and_takes_the_focus) {
    int clicks = 0;
    hike::Focus focus;
    auto ui = hike::row(
        hike::weight(1, hike::button("A")),
        hike::weight(1, hike::button("B").on_click([&] { ++clicks; }))
    );

    Ctx raw(20, 1);
    hike::Context ctx = hike::Context::adopt(raw.ctx);
    raw.ctx = nullptr;
    ui.draw(ctx, hike::Rect{0, 0, 20, 1}, focus);
    CHECK_EQ(focus.index(), 0);

    hike::Event ev{};
    ev.kind = HIKE_EVENT_MOUSE;
    ev.mouse.kind = HIKE_MOUSE_PRESS;
    ev.mouse.x = 15;     // inside the second child, which spans columns 10..19
    ev.mouse.y = 0;
    CHECK(ui.dispatch(ev, focus));
    CHECK_EQ(clicks, 1);
    CHECK_EQ(focus.index(), 1);   // the click moved the focus to what it hit
}

TEST(hike_cpp_split_matches_the_c_split) {
    hike::Rect area{0, 0, 10, 4};
    const std::vector<hike::Size> sizes = {hike::weight(1), hike::weight(1), hike::weight(1)};
    std::vector<hike::Rect> got = hike::split(hike_row(), area, sizes);

    const hike_size want_sizes[] = {hike_weight(1), hike_weight(1), hike_weight(1)};
    hike_rect want[3];
    hike_layout_split(hike_row(), area, want_sizes, 3, want);

    CHECK_EQ(int(got.size()), 3);
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(got[i].x, want[i].x);
        CHECK_EQ(got[i].w, want[i].w);
    }
}

TEST(hike_cpp_callbacks_run_on_activation) {
    int clicks = 0;
    bool checked_seen = false;
    hike::Button save = hike::button("Save").focused().on_click([&] { ++clicks; });
    hike::Event enter = key_event(HIKE_KEY_ENTER);
    CHECK(save.dispatch(hike::Rect{0, 0, 12, 1}, enter));
    CHECK_EQ(clicks, 1);

    hike::Checkbox wrap = hike::checkbox("Wrap").focused()
                              .on_change([&](bool v) { checked_seen = v; });
    hike::Event space = char_event(' ');
    CHECK(wrap.dispatch(hike::Rect{0, 0, 12, 1}, space));
    CHECK(wrap.is_checked());
    CHECK(checked_seen);
}

TEST(hike_cpp_input_owns_its_buffer_and_scrolls_like_the_c_widget) {
    Ctx raw(10, 1);
    hike::Context ctx = hike::Context::adopt(raw.ctx);
    raw.ctx = nullptr;

    hike::Input in(64);
    in.focused().text("abcdefghij");
    in.draw(ctx, hike::Rect{0, 0, 6, 1});
    CHECK(row_text(ctx.raw(), 0, 0, 6) == "fghij ");
    CHECK_EQ(in.scroll(), 5);
    CHECK(in.value() == "abcdefghij");

    hike::Event home = key_event(HIKE_KEY_HOME);
    CHECK(in.dispatch(6, home));
    in.draw(ctx, hike::Rect{0, 0, 6, 1});
    CHECK(row_text(ctx.raw(), 0, 0, 6) == "abcdef");
}

TEST(hike_cpp_input_survives_being_copied_into_a_tree) {
    // An Input's C struct points into the Input's own buffer, so a copy that
    // kept the original's pointer would read the original's storage and then,
    // once it died, freed memory. Copying is not exotic here: putting a widget
    // into a tree does it.
    hike::Input original(32);
    original.text("hello");
    hike::Input copy = original;
    original.text("gone");
    CHECK(copy.value() == "hello");

    hike::Input moved = std::move(copy);
    CHECK(moved.value() == "hello");

    Ctx raw(10, 1);
    hike::Context ctx = hike::Context::adopt(raw.ctx);
    raw.ctx = nullptr;
    moved.draw(ctx, hike::Rect{0, 0, 10, 1});
    CHECK(row_text(ctx.raw(), 0, 0, 6) == "hello ");
}

TEST(hike_cpp_list_reports_the_scroll_the_c_widget_chose) {
    Ctx raw(6, 2);
    hike::Context ctx = hike::Context::adopt(raw.ctx);
    raw.ctx = nullptr;

    hike::List l = hike::list().item("one").item("two").item("three").focused();
    hike::Event down = key_event(HIKE_KEY_DOWN);
    CHECK(l.dispatch(2, down));
    CHECK(l.dispatch(2, down));
    l.draw(ctx, hike::Rect{0, 0, 6, 2});
    CHECK_EQ(l.selection(), 2);
    CHECK_EQ(l.scroll_top(), 1);
    CHECK(row_text(ctx.raw(), 0, 0, 6) == "two   ");
    CHECK(row_text(ctx.raw(), 1, 0, 6) == "three ");
}

TEST(hike_cpp_focus_wraps_like_the_c_ring) {
    hike::Focus f(3);
    hike::Event tab = key_event(HIKE_KEY_TAB);
    hike::Event shift_tab = key_event(HIKE_KEY_TAB, 0, HIKE_MOD_SHIFT);
    CHECK(f.key(tab));
    CHECK(f.key(tab));
    CHECK(f.key(tab));
    CHECK_EQ(f.index(), 0);
    CHECK(f.key(shift_tab));
    CHECK_EQ(f.index(), 2);
    CHECK(f.has(2));
}

TEST(hike_cpp_focus_resize_keeps_the_index_where_it_can) {
    // A frame that adds a widget must not throw the user back to the first
    // field, and one that removes the focused widget must land somewhere real.
    hike::Focus f(3);
    f.set(2);
    f.resize(5);
    CHECK_EQ(f.index(), 2);
    f.resize(2);
    CHECK_EQ(f.index(), 1);
}

TEST(hike_cpp_context_restores_the_terminal_on_an_exception) {
    // The point of RAII here: the context is destroyed while the stack is
    // unwinding, so raw mode cannot outlive the error. A memory context has no
    // terminal to restore, but it does have memory to free, and running this
    // under a sanitiser is what proves the destructor ran.
    bool caught = false;
    try {
        hike::Context ctx = hike::Context::adopt(hike_context_new_memory(8, 2, HIKE_COLOR_16));
        hike::label("x").draw(ctx, hike::Rect{0, 0, 8, 1});
        throw std::runtime_error("boom");
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);
}

TEST(hike_cpp_error_carries_the_status_it_came_from) {
    bool caught = false;
    try {
        (void)hike::Context::adopt(nullptr);
    } catch (const hike::Error& e) {
        caught = true;
        CHECK(e.status() == HIKE_ERR_INVALID_ARGUMENT);
        CHECK(std::string(e.what()) == hike_status_text(HIKE_ERR_INVALID_ARGUMENT));
    }
    CHECK(caught);
}
