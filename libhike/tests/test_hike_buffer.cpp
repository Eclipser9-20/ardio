// Tests for libhike's unicode handling, cell buffer, and the present diff.
//
// None of these go through hike_init: it refuses when standard output is not a
// terminal, and a test runner's output is a pipe. They use the memory context
// instead, which is the same context over an in-memory sink, so the exact
// bytes a frame emits can be compared against a string written out by hand
// here rather than produced by the code under test.
//
// The escape sequences below are spelled out literally for that reason. A test
// that asked the library to format the expected sequence would pass whatever
// the library did.

#include "harness.h"

#include "../src/hike_internal.h"

#include <string>

namespace {

// The sink's contents as a string, so a test can compare and print it.
std::string sink(hike_context* ctx) {
    size_t len = 0;
    const char* p = hike_sink_data(ctx, &len);
    return std::string(p, len);
}

// A context of a known size with nothing on screen yet, already past its first
// full repaint so a test can measure what one change costs.
hike_context* fresh(int w, int h, hike_color_depth depth = HIKE_COLOR_TRUE) {
    hike_context* ctx = hike_context_new_memory(w, h, depth);
    hike_present(ctx); // the initial full repaint
    hike_sink_clear(ctx);
    return ctx;
}

hike_cell plain(uint32_t ch) {
    hike_cell c;
    c.ch = ch;
    c.fg = hike_default_color();
    c.bg = hike_default_color();
    c.attrs = 0;
    return c;
}

uint32_t decode1(const char* s) {
    uint32_t cp = 0;
    hike_utf8_decode(s, std::string(s).size(), &cp);
    return cp;
}

} // namespace

// ============================================================== unicode ====

TEST(utf8_round_trips_across_all_four_lengths) {
    const uint32_t points[] = {
        0x00, 'A', 0x7F,          // one byte
        0x80, 0xE9, 0x7FF,        // two
        0x800, 0x4E2D, 0xFFFD, 0xFFFF, // three
        0x10000, 0x1F600, 0x10FFFF     // four
    };
    for (uint32_t cp : points) {
        char buf[4];
        size_t n = hike_utf8_encode(cp, buf);
        CHECK(n >= 1 && n <= 4);
        uint32_t back = 0;
        CHECK_EQ(int(hike_utf8_decode(buf, n, &back)), int(n));
        CHECK_EQ(int(back), int(cp));
    }
}

TEST(utf8_encode_refuses_values_that_are_not_scalar_values) {
    char buf[4];
    CHECK_EQ(int(hike_utf8_encode(0xD800, buf)), 0);
    CHECK_EQ(int(hike_utf8_encode(0xDFFF, buf)), 0);
    CHECK_EQ(int(hike_utf8_encode(0x110000, buf)), 0);
    CHECK_EQ(int(hike_utf8_encode(0xFFFFFFFFu, buf)), 0);
}

TEST(utf8_decode_rejects_a_lone_continuation_byte) {
    uint32_t cp = 0;
    CHECK_EQ(int(hike_utf8_decode("\x80", 1, &cp)), 0);
    CHECK_EQ(int(hike_utf8_decode("\xBF", 1, &cp)), 0);
}

TEST(utf8_decode_rejects_the_withdrawn_five_and_six_byte_leads) {
    uint32_t cp = 0;
    CHECK_EQ(int(hike_utf8_decode("\xF8\x88\x80\x80\x80", 5, &cp)), 0);
    CHECK_EQ(int(hike_utf8_decode("\xFC\x84\x80\x80\x80\x80", 6, &cp)), 0);
    CHECK_EQ(int(hike_utf8_decode("\xFE", 1, &cp)), 0);
    CHECK_EQ(int(hike_utf8_decode("\xFF", 1, &cp)), 0);
}

TEST(utf8_decode_rejects_truncated_sequences) {
    uint32_t cp = 0;
    // Enough bytes present, but the buffer says otherwise: the decoder must
    // believe the length it was given and not read past it.
    CHECK_EQ(int(hike_utf8_decode("\xC3\xA9", 1, &cp)), 0);
    CHECK_EQ(int(hike_utf8_decode("\xE4\xB8\xAD", 2, &cp)), 0);
    CHECK_EQ(int(hike_utf8_decode("\xF0\x9F\x98\x80", 3, &cp)), 0);
    CHECK_EQ(int(hike_utf8_decode("", 0, &cp)), 0);
}

TEST(utf8_decode_rejects_a_missing_continuation_byte) {
    uint32_t cp = 0;
    CHECK_EQ(int(hike_utf8_decode("\xC3\x28", 2, &cp)), 0);
    CHECK_EQ(int(hike_utf8_decode("\xE4\xB8\x28", 3, &cp)), 0);
    CHECK_EQ(int(hike_utf8_decode("\xF0\x9F\x98\x28", 4, &cp)), 0);
}

TEST(utf8_decode_rejects_overlong_encodings) {
    uint32_t cp = 0;
    // Every one of these decodes arithmetically to a perfectly ordinary
    // character, which is exactly why accepting them is a security problem.
    CHECK_EQ(int(hike_utf8_decode("\xC0\x80", 2, &cp)), 0);         // NUL as 2
    CHECK_EQ(int(hike_utf8_decode("\xC1\xBF", 2, &cp)), 0);         // 0x7F as 2
    CHECK_EQ(int(hike_utf8_decode("\xE0\x80\x80", 3, &cp)), 0);     // NUL as 3
    CHECK_EQ(int(hike_utf8_decode("\xE0\x9F\xBF", 3, &cp)), 0);     // 0x7FF as 3
    CHECK_EQ(int(hike_utf8_decode("\xF0\x80\x80\x80", 4, &cp)), 0); // NUL as 4
    CHECK_EQ(int(hike_utf8_decode("\xF0\x8F\xBF\xBF", 4, &cp)), 0); // 0xFFFF as 4
}

TEST(utf8_decode_rejects_surrogates_and_out_of_range_values) {
    uint32_t cp = 0;
    CHECK_EQ(int(hike_utf8_decode("\xED\xA0\x80", 3, &cp)), 0); // U+D800
    CHECK_EQ(int(hike_utf8_decode("\xED\xBF\xBF", 3, &cp)), 0); // U+DFFF
    CHECK_EQ(int(hike_utf8_decode("\xF4\x90\x80\x80", 4, &cp)), 0); // U+110000
    CHECK_EQ(int(hike_utf8_decode("\xF7\xBF\xBF\xBF", 4, &cp)), 0); // way past
}

TEST(utf8_decode_accepts_the_boundaries_either_side_of_the_rejections) {
    uint32_t cp = 0;
    CHECK_EQ(int(hike_utf8_decode("\xED\x9F\xBF", 3, &cp)), 3); // U+D7FF
    CHECK_EQ(int(cp), 0xD7FF);
    CHECK_EQ(int(hike_utf8_decode("\xEE\x80\x80", 3, &cp)), 3); // U+E000
    CHECK_EQ(int(cp), 0xE000);
    CHECK_EQ(int(hike_utf8_decode("\xF4\x8F\xBF\xBF", 4, &cp)), 4); // U+10FFFF
    CHECK_EQ(int(cp), 0x10FFFF);
}

TEST(char_width_ascii_combining_cjk_and_emoji) {
    CHECK_EQ(hike_char_width('A'), 1);
    CHECK_EQ(hike_char_width(' '), 1);
    CHECK_EQ(hike_char_width(0x00E9), 1);   // e with acute, precomposed
    CHECK_EQ(hike_char_width(0x0301), 0);   // combining acute accent
    CHECK_EQ(hike_char_width(0x200B), 0);   // zero width space
    CHECK_EQ(hike_char_width(0xFE0F), 0);   // variation selector 16
    CHECK_EQ(hike_char_width(0x4E2D), 2);   // CJK ideograph
    CHECK_EQ(hike_char_width(0x3042), 2);   // hiragana A
    CHECK_EQ(hike_char_width(0xFF21), 2);   // fullwidth A
    CHECK_EQ(hike_char_width(0x1F600), 2);  // grinning face
    CHECK_EQ(hike_char_width(0x1F680), 2);  // rocket
    CHECK_EQ(hike_char_width('\n'), 0);     // controls hold no column
}

TEST(text_width_counts_columns_not_bytes_or_code_points) {
    CHECK_EQ(hike_text_width(""), 0);
    CHECK_EQ(hike_text_width("hello"), 5);
    // Four bytes, two code points, one column: a base letter and its mark.
    CHECK_EQ(hike_text_width("e\xCC\x81"), 1);
    // Nine bytes, three code points, six columns.
    CHECK_EQ(hike_text_width("\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97"), 6);
    // Mixed: four ASCII columns plus one two-column emoji.
    CHECK_EQ(hike_text_width("ab\xF0\x9F\x9A\x80" "cd"), 6);
}

// =============================================================== buffer ====

TEST(rect_contains_and_intersect) {
    hike_rect r{2, 3, 4, 5};
    CHECK(hike_rect_contains(r, 2, 3));
    CHECK(hike_rect_contains(r, 5, 7));
    CHECK(!hike_rect_contains(r, 6, 7));
    CHECK(!hike_rect_contains(r, 2, 8));
    CHECK(!hike_rect_contains(r, 1, 3));

    hike_rect empty{0, 0, 0, 0};
    CHECK(!hike_rect_contains(empty, 0, 0));

    hike_rect a{0, 0, 10, 10};
    hike_rect b{5, 5, 10, 10};
    hike_rect i = hike_rect_intersect(a, b);
    CHECK_EQ(i.x, 5); CHECK_EQ(i.y, 5); CHECK_EQ(i.w, 5); CHECK_EQ(i.h, 5);

    // Disjoint rectangles come back empty rather than negative.
    hike_rect c{100, 100, 3, 3};
    hike_rect j = hike_rect_intersect(a, c);
    CHECK_EQ(j.w, 0); CHECK_EQ(j.h, 0);
}

TEST(colors_construct_and_compare_by_kind) {
    hike_color d = hike_default_color();
    CHECK(d.kind == HIKE_COLOR_DEFAULT);
    hike_color i = hike_indexed(200);
    CHECK(i.kind == HIKE_COLOR_INDEXED);
    CHECK_EQ(int(i.index), 200);
    hike_color r = hike_rgb(1, 2, 3);
    CHECK(r.kind == HIKE_COLOR_RGB);
    CHECK_EQ(int(r.r), 1); CHECK_EQ(int(r.g), 2); CHECK_EQ(int(r.b), 3);
}

TEST(buffer_starts_blank_and_set_get_round_trip) {
    hike_context* ctx = hike_context_new_memory(10, 4, HIKE_COLOR_TRUE);
    CHECK_EQ(hike_width(ctx), 10);
    CHECK_EQ(hike_height(ctx), 4);
    CHECK(hike_depth(ctx) == HIKE_COLOR_TRUE);
    CHECK_EQ(int(hike_get_cell(ctx, 0, 0).ch), int(' '));

    hike_cell c = plain('X');
    c.fg = hike_rgb(10, 20, 30);
    c.attrs = HIKE_BOLD;
    hike_set_cell(ctx, 3, 2, c);
    hike_cell got = hike_get_cell(ctx, 3, 2);
    CHECK_EQ(int(got.ch), int('X'));
    CHECK_EQ(int(got.attrs), int(HIKE_BOLD));
    CHECK_EQ(int(got.fg.g), 20);

    // Out of bounds neither writes nor reads anything real.
    hike_set_cell(ctx, 100, 100, plain('Z'));
    CHECK_EQ(int(hike_get_cell(ctx, 100, 100).ch), int(' '));
    CHECK_EQ(int(hike_get_cell(ctx, -1, 0).ch), int(' '));

    hike_clear(ctx);
    CHECK_EQ(int(hike_get_cell(ctx, 3, 2).ch), int(' '));
    hike_context_free(ctx);
}

TEST(text_returns_columns_advanced_not_bytes) {
    hike_context* ctx = hike_context_new_memory(20, 3, HIKE_COLOR_TRUE);
    hike_color fg = hike_default_color();

    CHECK_EQ(hike_text(ctx, 0, 0, "hi", fg, fg, 0), 2);
    // Three bytes, one code point, two columns.
    CHECK_EQ(hike_text(ctx, 0, 1, "\xE4\xB8\xAD", fg, fg, 0), 2);
    // Four bytes, one code point, two columns.
    CHECK_EQ(hike_text(ctx, 0, 2, "\xF0\x9F\x9A\x80", fg, fg, 0), 2);
    // The combining mark takes no column of its own.
    CHECK_EQ(hike_text(ctx, 4, 0, "e\xCC\x81", fg, fg, 0), 1);
    CHECK_EQ(int(hike_get_cell(ctx, 4, 0).ch), int('e'));
    CHECK_EQ(int(hike_get_cell(ctx, 5, 0).ch), int(' '));
    hike_context_free(ctx);
}

TEST(text_draws_a_replacement_for_invalid_bytes_and_keeps_counting) {
    hike_context* ctx = hike_context_new_memory(20, 2, HIKE_COLOR_TRUE);
    hike_color fg = hike_default_color();
    // One bad byte between two good letters: three columns, and the bad byte
    // is visible rather than silently dropped.
    CHECK_EQ(hike_text(ctx, 0, 0, "a\xFFz", fg, fg, 0), 3);
    CHECK_EQ(int(hike_get_cell(ctx, 0, 0).ch), int('a'));
    CHECK_EQ(int(hike_get_cell(ctx, 1, 0).ch), 0xFFFD);
    CHECK_EQ(int(hike_get_cell(ctx, 2, 0).ch), int('z'));
    hike_context_free(ctx);
}

TEST(a_wide_character_occupies_two_cells) {
    hike_context* ctx = hike_context_new_memory(10, 2, HIKE_COLOR_TRUE);
    hike_color fg = hike_default_color();
    hike_text(ctx, 1, 0, "\xE4\xB8\xAD", fg, fg, 0);

    CHECK_EQ(int(hike_get_cell(ctx, 1, 0).ch), 0x4E2D);
    CHECK_EQ(int(hike_get_cell(ctx, 2, 0).ch), int(HIKE_CELL_CONTINUATION));
    CHECK_EQ(int(hike_get_cell(ctx, 3, 0).ch), int(' '));
    hike_context_free(ctx);
}

TEST(writing_over_either_half_of_a_wide_pair_blanks_the_other) {
    hike_context* ctx = hike_context_new_memory(10, 2, HIKE_COLOR_TRUE);
    hike_color fg = hike_default_color();

    // Over the lead: the orphaned continuation becomes a blank.
    hike_text(ctx, 1, 0, "\xE4\xB8\xAD", fg, fg, 0);
    hike_set_cell(ctx, 1, 0, plain('A'));
    CHECK_EQ(int(hike_get_cell(ctx, 1, 0).ch), int('A'));
    CHECK_EQ(int(hike_get_cell(ctx, 2, 0).ch), int(' '));

    // Over the continuation: the orphaned lead becomes a blank.
    hike_text(ctx, 4, 0, "\xE4\xB8\xAD", fg, fg, 0);
    hike_set_cell(ctx, 5, 0, plain('B'));
    CHECK_EQ(int(hike_get_cell(ctx, 4, 0).ch), int(' '));
    CHECK_EQ(int(hike_get_cell(ctx, 5, 0).ch), int('B'));
    hike_context_free(ctx);
}

TEST(a_continuation_cannot_be_written_directly_and_a_wide_char_needs_both_columns) {
    hike_context* ctx = hike_context_new_memory(4, 1, HIKE_COLOR_TRUE);
    hike_set_cell(ctx, 0, 0, plain(HIKE_CELL_CONTINUATION));
    CHECK_EQ(int(hike_get_cell(ctx, 0, 0).ch), int(' '));

    // The last column has no room for a second half, so nothing is written
    // rather than half a glyph.
    hike_set_cell(ctx, 3, 0, plain(0x4E2D));
    CHECK_EQ(int(hike_get_cell(ctx, 3, 0).ch), int(' '));
    hike_context_free(ctx);
}

TEST(fill_writes_only_inside_the_rectangle) {
    hike_context* ctx = hike_context_new_memory(8, 4, HIKE_COLOR_TRUE);
    hike_rect r{1, 1, 3, 2};
    hike_fill(ctx, r, plain('#'));
    CHECK_EQ(int(hike_get_cell(ctx, 1, 1).ch), int('#'));
    CHECK_EQ(int(hike_get_cell(ctx, 3, 2).ch), int('#'));
    CHECK_EQ(int(hike_get_cell(ctx, 0, 1).ch), int(' '));
    CHECK_EQ(int(hike_get_cell(ctx, 4, 1).ch), int(' '));
    CHECK_EQ(int(hike_get_cell(ctx, 1, 3).ch), int(' '));
    hike_context_free(ctx);
}

TEST(clipping_discards_writes_outside_the_clip) {
    hike_context* ctx = hike_context_new_memory(10, 3, HIKE_COLOR_TRUE);
    hike_color fg = hike_default_color();

    hike_rect r{2, 0, 3, 1};
    hike_push_clip(ctx, r);
    // Six columns of text, three of which land.
    CHECK_EQ(hike_text(ctx, 0, 0, "abcdef", fg, fg, 0), 6);
    hike_pop_clip(ctx);

    CHECK_EQ(int(hike_get_cell(ctx, 0, 0).ch), int(' '));
    CHECK_EQ(int(hike_get_cell(ctx, 1, 0).ch), int(' '));
    CHECK_EQ(int(hike_get_cell(ctx, 2, 0).ch), int('c'));
    CHECK_EQ(int(hike_get_cell(ctx, 4, 0).ch), int('e'));
    CHECK_EQ(int(hike_get_cell(ctx, 5, 0).ch), int(' '));

    // The clip is gone once popped.
    hike_set_cell(ctx, 0, 0, plain('Z'));
    CHECK_EQ(int(hike_get_cell(ctx, 0, 0).ch), int('Z'));
    hike_context_free(ctx);
}

TEST(nested_clips_intersect_and_pop_restores_the_parent) {
    hike_context* ctx = hike_context_new_memory(10, 4, HIKE_COLOR_TRUE);

    hike_rect outer{0, 0, 6, 4};
    hike_rect inner{4, 0, 6, 4}; // reaches past the outer clip on the right
    hike_push_clip(ctx, outer);
    hike_push_clip(ctx, inner);

    // The child can only narrow: columns 4 and 5 survive, 6 does not.
    hike_set_cell(ctx, 4, 0, plain('a'));
    hike_set_cell(ctx, 6, 0, plain('b'));
    CHECK_EQ(int(hike_get_cell(ctx, 4, 0).ch), int('a'));
    CHECK_EQ(int(hike_get_cell(ctx, 6, 0).ch), int(' '));

    hike_pop_clip(ctx);
    // Back to the outer clip: column 0 is allowed again, column 6 still is not.
    hike_set_cell(ctx, 0, 0, plain('c'));
    hike_set_cell(ctx, 6, 1, plain('d'));
    CHECK_EQ(int(hike_get_cell(ctx, 0, 0).ch), int('c'));
    CHECK_EQ(int(hike_get_cell(ctx, 6, 1).ch), int(' '));

    hike_pop_clip(ctx);
    hike_set_cell(ctx, 9, 3, plain('e'));
    CHECK_EQ(int(hike_get_cell(ctx, 9, 3).ch), int('e'));

    // An unbalanced pop must not remove the screen clip itself.
    hike_pop_clip(ctx);
    hike_pop_clip(ctx);
    hike_set_cell(ctx, 8, 3, plain('f'));
    CHECK_EQ(int(hike_get_cell(ctx, 8, 3).ch), int('f'));
    hike_context_free(ctx);
}

TEST(a_fully_off_screen_clip_discards_everything) {
    hike_context* ctx = hike_context_new_memory(6, 2, HIKE_COLOR_TRUE);
    hike_color fg = hike_default_color();

    hike_rect gone{40, 40, 5, 5};
    hike_push_clip(ctx, gone);
    // The column count still describes the text, because a caller laying out
    // a line needs the same answer whether or not it was visible.
    CHECK_EQ(hike_text(ctx, 0, 0, "abc", fg, fg, 0), 3);
    hike_fill(ctx, hike_rect{0, 0, 6, 2}, plain('#'));
    hike_pop_clip(ctx);

    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 6; ++x)
            CHECK_EQ(int(hike_get_cell(ctx, x, y).ch), int(' '));
    hike_context_free(ctx);
}

// ================================================================= diff ====

TEST(the_first_present_paints_everything_and_the_next_paints_nothing) {
    hike_context* ctx = hike_context_new_memory(4, 2, HIKE_COLOR_TRUE);
    hike_present(ctx);
    CHECK(sink(ctx).size() > 0);

    hike_sink_clear(ctx);
    hike_present(ctx);
    CHECK_EQ(int(sink(ctx).size()), 0);
    hike_context_free(ctx);
}

TEST(one_changed_cell_emits_only_that_run) {
    hike_context* ctx = fresh(10, 3);
    hike_set_cell(ctx, 4, 1, plain('X'));
    hike_present(ctx);

    // Move to row 2 column 5 on the wire, reset the style, write the one
    // character, and reset again at the end of the frame.
    CHECK(sink(ctx) == "\x1b[2;5H\x1b[0mX\x1b[0m");
    hike_context_free(ctx);
}

TEST(adjacent_changes_become_one_run_with_one_move_and_one_style) {
    hike_context* ctx = fresh(10, 2);
    hike_color fg = hike_default_color();
    hike_text(ctx, 2, 0, "abc", fg, fg, 0);
    hike_present(ctx);
    CHECK(sink(ctx) == "\x1b[1;3H\x1b[0mabc\x1b[0m");

    // Two separate runs on the same row need two moves but, since the style is
    // unchanged between them, only one style sequence.
    hike_sink_clear(ctx);
    hike_set_cell(ctx, 0, 0, plain('L'));
    hike_set_cell(ctx, 8, 0, plain('R'));
    hike_present(ctx);
    CHECK(sink(ctx) == "\x1b[1;1H\x1b[0mL\x1b[1;9HR\x1b[0m");
    hike_context_free(ctx);
}

TEST(a_style_change_inside_a_run_is_emitted_once) {
    hike_context* ctx = fresh(10, 1);
    hike_color def = hike_default_color();
    hike_text(ctx, 0, 0, "ab", def, def, 0);
    hike_text(ctx, 2, 0, "cd", hike_rgb(255, 0, 0), def, HIKE_BOLD);
    hike_present(ctx);
    CHECK(sink(ctx) == "\x1b[1;1H\x1b[0mab\x1b[0;1;38;2;255;0;0mcd\x1b[0m");
    hike_context_free(ctx);
}

TEST(a_wide_character_is_written_once_and_its_continuation_never_is) {
    hike_context* ctx = fresh(6, 1);
    hike_color fg = hike_default_color();
    hike_text(ctx, 1, 0, "\xE4\xB8\xAD", fg, fg, 0);
    hike_present(ctx);
    // One move, one style, the three UTF-8 bytes of the one character, reset.
    CHECK(sink(ctx) == "\x1b[1;2H\x1b[0m\xE4\xB8\xAD\x1b[0m");

    // And a second present with nothing changed writes nothing, which is the
    // check that the continuation cell was reconciled too.
    hike_sink_clear(ctx);
    hike_present(ctx);
    CHECK_EQ(int(sink(ctx).size()), 0);
    hike_context_free(ctx);
}

TEST(invalidate_forces_a_full_repaint) {
    hike_context* ctx = fresh(3, 2);
    hike_present(ctx);
    CHECK_EQ(int(sink(ctx).size()), 0);

    hike_invalidate(ctx);
    hike_present(ctx);
    std::string s = sink(ctx);
    // Every cell of both rows, so both rows are addressed and six blanks go
    // out even though not one of them changed.
    CHECK(s == "\x1b[1;1H\x1b[0m   \x1b[2;1H   \x1b[0m");

    // And the force is spent: the frame after it is silent again.
    hike_sink_clear(ctx);
    hike_present(ctx);
    CHECK_EQ(int(sink(ctx).size()), 0);
    hike_context_free(ctx);
}

TEST(the_cursor_is_emitted_only_when_it_changes) {
    hike_context* ctx = fresh(8, 2);
    hike_set_cursor(ctx, 3, 1, true);
    hike_present(ctx);
    CHECK(sink(ctx) == "\x1b[2;4H\x1b[?25h");

    hike_sink_clear(ctx);
    hike_present(ctx);
    CHECK_EQ(int(sink(ctx).size()), 0);

    hike_sink_clear(ctx);
    hike_set_cursor(ctx, 0, 0, false);
    hike_present(ctx);
    CHECK(sink(ctx) == "\x1b[?25l");
    hike_context_free(ctx);
}

// ================================================== colour reduction ======

TEST(rgb_reduces_to_256_through_the_cube_and_the_grey_ramp) {
    // Cube corners: index 16 is black and 231 is white.
    CHECK_EQ(int(hike_color_to_256(hike_rgb(0, 0, 0))), 16);
    CHECK_EQ(int(hike_color_to_256(hike_rgb(255, 255, 255))), 231);
    // Pure red is level 5 on the red axis and 0 elsewhere: 16 + 36*5.
    CHECK_EQ(int(hike_color_to_256(hike_rgb(255, 0, 0))), 196);
    CHECK_EQ(int(hike_color_to_256(hike_rgb(0, 255, 0))), 46);   // 16 + 6*5
    CHECK_EQ(int(hike_color_to_256(hike_rgb(0, 0, 255))), 21);   // 16 + 5
    // A mid grey belongs on the 24-step ramp, not in the cube.
    CHECK_EQ(int(hike_color_to_256(hike_rgb(128, 128, 128))), 244); // 232 + 12
    // Near-white and near-black greys clamp to the cube's ends.
    CHECK_EQ(int(hike_color_to_256(hike_rgb(2, 2, 2))), 16);
    CHECK_EQ(int(hike_color_to_256(hike_rgb(252, 252, 252))), 231);
    // An indexed colour is already in the palette and passes through.
    CHECK_EQ(int(hike_color_to_256(hike_indexed(123))), 123);
}

TEST(rgb_and_256_reduce_to_the_sixteen_ansi_colours) {
    CHECK_EQ(int(hike_color_to_16(hike_rgb(0, 0, 0))), 0);
    CHECK_EQ(int(hike_color_to_16(hike_rgb(255, 255, 255))), 15);
    CHECK_EQ(int(hike_color_to_16(hike_rgb(255, 0, 0))), 9);    // bright red
    CHECK_EQ(int(hike_color_to_16(hike_rgb(160, 0, 0))), 1);    // dim red
    // Nearest to xterm's own blue (0, 0, 238), not to its brighter one.
    CHECK_EQ(int(hike_color_to_16(hike_rgb(0, 0, 250))), 4);
    CHECK_EQ(int(hike_color_to_16(hike_rgb(200, 200, 200))), 7);
    CHECK_EQ(int(hike_color_to_16(hike_rgb(130, 130, 130))), 8);
    // The low sixteen palette entries are themselves.
    CHECK_EQ(int(hike_color_to_16(hike_indexed(5))), 5);
    // A high palette index goes through its RGB value to get there.
    CHECK_EQ(int(hike_color_to_16(hike_indexed(196))), 9);  // cube's pure red
    CHECK_EQ(int(hike_color_to_16(hike_indexed(232))), 0);  // darkest grey
}

TEST(the_terminal_depth_decides_which_colour_sequence_is_written) {
    hike_color red = hike_rgb(255, 0, 0);
    hike_color def = hike_default_color();

    hike_context* t = fresh(3, 1, HIKE_COLOR_TRUE);
    hike_text(t, 0, 0, "x", red, def, 0);
    hike_present(t);
    CHECK(sink(t) == "\x1b[1;1H\x1b[0;38;2;255;0;0mx\x1b[0m");
    hike_context_free(t);

    hike_context* c256 = fresh(3, 1, HIKE_COLOR_256);
    hike_text(c256, 0, 0, "x", red, def, 0);
    hike_present(c256);
    CHECK(sink(c256) == "\x1b[1;1H\x1b[0;38;5;196mx\x1b[0m");
    hike_context_free(c256);

    hike_context* c16 = fresh(3, 1, HIKE_COLOR_16);
    hike_text(c16, 0, 0, "x", red, hike_rgb(0, 0, 0), 0);
    hike_present(c16);
    CHECK(sink(c16) == "\x1b[1;1H\x1b[0;91;40mx\x1b[0m");
    hike_context_free(c16);

    hike_context* none = fresh(3, 1, HIKE_COLOR_NONE);
    hike_text(none, 0, 0, "x", red, def, HIKE_UNDERLINE);
    hike_present(none);
    CHECK(sink(none) == "\x1b[1;1H\x1b[0;4mx\x1b[0m");
    hike_context_free(none);
}

TEST(status_text_names_every_status_and_defaults_are_sane) {
    CHECK(std::string(hike_status_text(HIKE_OK)) == "ok");
    for (int s = HIKE_OK; s <= HIKE_ERR_INVALID_ARGUMENT; ++s)
        CHECK(std::string(hike_status_text((hike_status)s)).size() > 0);

    hike_options o = hike_default_options();
    CHECK(o.alternate_screen);
    CHECK(o.hide_cursor);
    CHECK(!o.mouse);
    CHECK(!o.bracketed_paste);
    CHECK(!o.focus_events);
}

TEST(init_refuses_when_output_is_not_a_terminal) {
    // The test runner's output is a pipe or a file, never a terminal, so this
    // is the one thing about hike_init that can be checked in CI. Everything
    // past the check needs a real tty and is exercised by hand instead.
    hike_context* ctx = (hike_context*)0x1;
    hike_options o = hike_default_options();
    hike_status st = hike_init(&ctx, &o);
    if (st == HIKE_OK) {
        // Someone ran the suite attached to a terminal. Put it back.
        hike_shutdown(ctx);
    } else {
        CHECK(st == HIKE_ERR_NOT_A_TERMINAL);
        CHECK(ctx == nullptr);
    }
    CHECK(hike_init(nullptr, &o) == HIKE_ERR_INVALID_ARGUMENT);
}

TEST(decoding_helper_sanity) {
    // Guards the tests above: if this reads wrong, the width expectations that
    // depend on the same literals are testing the wrong characters.
    CHECK_EQ(int(decode1("\xE4\xB8\xAD")), 0x4E2D);
    CHECK_EQ(int(decode1("\xF0\x9F\x9A\x80")), 0x1F680);
    CHECK_EQ(int(decode1("\xCC\x81")), 0x0301);
}
