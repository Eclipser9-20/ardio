// Tests for libhike's terminal input parser.
//
// These drive the parser directly with bytes rather than through hike_poll,
// because a real terminal cannot be scripted from a test suite: there is no
// way to make a tty deliver "ESC [ 1 ; 5 A" split after the semicolon at a
// chosen moment, and that split is exactly the case worth testing. Feeding the
// state machine by hand tests the same code a terminal would reach, and adds
// control over chunk boundaries that a terminal would never give.
//
// Byte sequences are written out literally, as a terminal emits them, instead
// of being built from the same constants the parser uses. A test that asks the
// implementation what Ctrl+Up looks like agrees with it by construction and
// checks nothing.

#include "harness.h"

extern "C" {
#include "../src/hike_input_internal.h"
}

#include <cstring>
#include <string>
#include <vector>

namespace {

// A parser with its lifetime tied to the test, plus the two operations every
// test below needs: push bytes in, pull events out.
class Parser {
public:
    Parser() { hike_input_init(&p_); }
    ~Parser() { hike_input_free(&p_); }
    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    void feed(const std::string& bytes) {
        hike_input_feed(&p_, bytes.data(), bytes.size());
    }

    // Takes the next event, or an event of kind NONE when the bytes so far do
    // not make one. `flush` stands in for the escape timeout expiring.
    hike_event next(bool flush = false) {
        hike_event e;
        std::memset(&e, 0, sizeof(e));
        if (!hike_input_next(&p_, &e, flush)) e.kind = HIKE_EVENT_NONE;
        return e;
    }

    // The common case: feed a whole sequence and read the one event it makes.
    hike_event one(const std::string& bytes, bool flush = false) {
        feed(bytes);
        return next(flush);
    }

    bool drained() { return next(false).kind == HIKE_EVENT_NONE; }

private:
    hike_input_parser p_;
};

bool is_key(const hike_event& e, hike_key key, uint8_t mods = 0) {
    return e.kind == HIKE_EVENT_KEY && e.key.key == key && e.key.mods == mods;
}

bool is_char(const hike_event& e, uint32_t ch, uint8_t mods = 0) {
    return e.kind == HIKE_EVENT_KEY && e.key.key == HIKE_KEY_CHAR &&
           e.key.ch == ch && e.key.mods == mods;
}

const char kEsc = '\x1b';

std::string csi(const std::string& body) { return std::string(1, kEsc) + "[" + body; }
std::string ss3(const std::string& body) { return std::string(1, kEsc) + "O" + body; }

} // namespace

// ================================================================= text ====

TEST(hike_input_plain_ascii_is_one_event_per_character) {
    Parser p;
    p.feed("hi!");
    CHECK(is_char(p.next(), 'h'));
    CHECK(is_char(p.next(), 'i'));
    CHECK(is_char(p.next(), '!'));
    CHECK(p.drained());
}

TEST(hike_input_multibyte_utf8_arriving_whole) {
    Parser p;
    CHECK(is_char(p.one("\xC3\xA9"), 0xE9));         // e-acute
    CHECK(is_char(p.one("\xE2\x82\xAC"), 0x20AC));   // euro sign
    CHECK(is_char(p.one("\xF0\x9F\x8E\x88"), 0x1F388));  // four-byte
}

TEST(hike_input_multibyte_utf8_split_one_byte_per_feed) {
    // The whole point of a resumable parser. Each byte arrives in its own read,
    // and nothing may be emitted until the character is complete.
    Parser p;
    const char* bytes = "\xE2\x82\xAC";
    p.feed(std::string(1, bytes[0]));
    CHECK_EQ(int(p.next().kind), int(HIKE_EVENT_NONE));
    p.feed(std::string(1, bytes[1]));
    CHECK_EQ(int(p.next().kind), int(HIKE_EVENT_NONE));
    p.feed(std::string(1, bytes[2]));
    CHECK(is_char(p.next(), 0x20AC));
    CHECK(p.drained());
}

TEST(hike_input_invalid_utf8_is_dropped_without_losing_what_follows) {
    Parser p;
    p.feed("\xFF" "A");
    // The lead byte is impossible, so it goes; the A after it must survive.
    CHECK(is_char(p.next(), 'A'));
    CHECK(p.drained());
}

// ============================================================= controls ====

TEST(hike_input_enter_tab_and_both_backspace_bytes) {
    Parser p;
    CHECK(is_key(p.one("\r"), HIKE_KEY_ENTER));
    CHECK(is_key(p.one("\n"), HIKE_KEY_ENTER));
    CHECK(is_key(p.one("\t"), HIKE_KEY_TAB));
    // Which byte Backspace sends is a property of the terminal, not the key.
    CHECK(is_key(p.one("\x7f"), HIKE_KEY_BACKSPACE));
    CHECK(is_key(p.one("\x08"), HIKE_KEY_BACKSPACE));
}

TEST(hike_input_ctrl_letter_reports_the_letter) {
    Parser p;
    CHECK(is_char(p.one("\x01"), 'a', HIKE_MOD_CTRL));
    CHECK(is_char(p.one("\x03"), 'c', HIKE_MOD_CTRL));
    CHECK(is_char(p.one("\x1a"), 'z', HIKE_MOD_CTRL));
    CHECK(is_char(p.one(std::string(1, '\0')), ' ', HIKE_MOD_CTRL));
}

TEST(hike_input_ctrl_letters_that_collide_with_named_keys_stay_named) {
    // Ctrl+I and Ctrl+M are byte-identical to Tab and Enter. The terminal has
    // already thrown the distinction away, so the named key is the honest
    // reading and guessing the other would be wrong more often.
    Parser p;
    CHECK(is_key(p.one("\x09"), HIKE_KEY_TAB));
    CHECK(is_key(p.one("\x0d"), HIKE_KEY_ENTER));
}

// ============================================================ named keys ====

TEST(hike_input_arrows_in_csi_form) {
    Parser p;
    CHECK(is_key(p.one(csi("A")), HIKE_KEY_UP));
    CHECK(is_key(p.one(csi("B")), HIKE_KEY_DOWN));
    CHECK(is_key(p.one(csi("C")), HIKE_KEY_RIGHT));
    CHECK(is_key(p.one(csi("D")), HIKE_KEY_LEFT));
}

TEST(hike_input_arrows_in_ss3_form) {
    // Application cursor mode sends the same keys through SS3. A caller must
    // not have to care which mode the terminal happens to be in.
    Parser p;
    CHECK(is_key(p.one(ss3("A")), HIKE_KEY_UP));
    CHECK(is_key(p.one(ss3("B")), HIKE_KEY_DOWN));
    CHECK(is_key(p.one(ss3("C")), HIKE_KEY_RIGHT));
    CHECK(is_key(p.one(ss3("D")), HIKE_KEY_LEFT));
}

TEST(hike_input_home_and_end_in_all_three_forms) {
    Parser p;
    CHECK(is_key(p.one(csi("H")), HIKE_KEY_HOME));
    CHECK(is_key(p.one(csi("F")), HIKE_KEY_END));
    CHECK(is_key(p.one(ss3("H")), HIKE_KEY_HOME));
    CHECK(is_key(p.one(ss3("F")), HIKE_KEY_END));
    CHECK(is_key(p.one(csi("1~")), HIKE_KEY_HOME));
    CHECK(is_key(p.one(csi("4~")), HIKE_KEY_END));
    CHECK(is_key(p.one(csi("7~")), HIKE_KEY_HOME));
    CHECK(is_key(p.one(csi("8~")), HIKE_KEY_END));
}

TEST(hike_input_navigation_cluster) {
    Parser p;
    CHECK(is_key(p.one(csi("2~")), HIKE_KEY_INSERT));
    CHECK(is_key(p.one(csi("3~")), HIKE_KEY_DELETE));
    CHECK(is_key(p.one(csi("5~")), HIKE_KEY_PAGE_UP));
    CHECK(is_key(p.one(csi("6~")), HIKE_KEY_PAGE_DOWN));
}

TEST(hike_input_f1_to_f4_in_both_ss3_and_csi_forms) {
    Parser p;
    CHECK(is_key(p.one(ss3("P")), HIKE_KEY_F1));
    CHECK(is_key(p.one(ss3("Q")), HIKE_KEY_F2));
    CHECK(is_key(p.one(ss3("R")), HIKE_KEY_F3));
    CHECK(is_key(p.one(ss3("S")), HIKE_KEY_F4));
    CHECK(is_key(p.one(csi("P")), HIKE_KEY_F1));
    CHECK(is_key(p.one(csi("Q")), HIKE_KEY_F2));
    CHECK(is_key(p.one(csi("R")), HIKE_KEY_F3));
    CHECK(is_key(p.one(csi("S")), HIKE_KEY_F4));
    CHECK(is_key(p.one(csi("11~")), HIKE_KEY_F1));
    CHECK(is_key(p.one(csi("12~")), HIKE_KEY_F2));
    CHECK(is_key(p.one(csi("13~")), HIKE_KEY_F3));
    CHECK(is_key(p.one(csi("14~")), HIKE_KEY_F4));
}

TEST(hike_input_f5_to_f12_numbering_skips_16_and_22) {
    // The gaps are inherited from the VT220 keypad and are easy to get wrong,
    // so every one is spelled out rather than generated from a loop.
    Parser p;
    CHECK(is_key(p.one(csi("15~")), HIKE_KEY_F5));
    CHECK(is_key(p.one(csi("17~")), HIKE_KEY_F6));
    CHECK(is_key(p.one(csi("18~")), HIKE_KEY_F7));
    CHECK(is_key(p.one(csi("19~")), HIKE_KEY_F8));
    CHECK(is_key(p.one(csi("20~")), HIKE_KEY_F9));
    CHECK(is_key(p.one(csi("21~")), HIKE_KEY_F10));
    CHECK(is_key(p.one(csi("23~")), HIKE_KEY_F11));
    CHECK(is_key(p.one(csi("24~")), HIKE_KEY_F12));
    // 16 and 22 are not keys. They must vanish, not become an adjacent one.
    CHECK_EQ(int(p.one(csi("16~")).kind), int(HIKE_EVENT_NONE));
    CHECK_EQ(int(p.one(csi("22~")).kind), int(HIKE_EVENT_NONE));
}

// ============================================================ modifiers ====

TEST(hike_input_modified_arrows) {
    Parser p;
    CHECK(is_key(p.one(csi("1;5A")), HIKE_KEY_UP, HIKE_MOD_CTRL));
    CHECK(is_key(p.one(csi("1;2B")), HIKE_KEY_DOWN, HIKE_MOD_SHIFT));
    CHECK(is_key(p.one(csi("1;3C")), HIKE_KEY_RIGHT, HIKE_MOD_ALT));
    CHECK(is_key(p.one(csi("1;6D")), HIKE_KEY_LEFT,
                 HIKE_MOD_SHIFT | HIKE_MOD_CTRL));
    CHECK(is_key(p.one(csi("1;8A")), HIKE_KEY_UP,
                 HIKE_MOD_SHIFT | HIKE_MOD_ALT | HIKE_MOD_CTRL));
}

TEST(hike_input_modifiers_on_tilde_and_ss3_forms) {
    Parser p;
    CHECK(is_key(p.one(csi("3;5~")), HIKE_KEY_DELETE, HIKE_MOD_CTRL));
    CHECK(is_key(p.one(csi("5;2~")), HIKE_KEY_PAGE_UP, HIKE_MOD_SHIFT));
    CHECK(is_key(p.one(csi("15;3~")), HIKE_KEY_F5, HIKE_MOD_ALT));
    // Some terminals put the modifier inside the SS3 form instead.
    CHECK(is_key(p.one(ss3("1;5A")), HIKE_KEY_UP, HIKE_MOD_CTRL));
}

TEST(hike_input_alt_is_an_escape_prefix) {
    Parser p;
    CHECK(is_char(p.one("\x1b" "a"), 'a', HIKE_MOD_ALT));
    CHECK(is_char(p.one("\x1b" "Z"), 'Z', HIKE_MOD_ALT));
    // Alt with a control byte keeps both modifiers.
    CHECK(is_char(p.one("\x1b\x03"), 'c', HIKE_MOD_ALT | HIKE_MOD_CTRL));
    CHECK(is_key(p.one("\x1b\x7f"), HIKE_KEY_BACKSPACE, HIKE_MOD_ALT));
    // Alt with a multi-byte character.
    CHECK(is_char(p.one("\x1b\xC3\xA9"), 0xE9, HIKE_MOD_ALT));
}

TEST(hike_input_lone_escape_needs_the_timeout_to_resolve) {
    // A lone ESC and the first byte of an arrow key are the same byte, so with
    // nothing after it the parser must not commit: it says "no event yet" and
    // only calls it Escape once the caller reports the timeout expired.
    Parser p;
    p.feed("\x1b");
    CHECK_EQ(int(p.next(false).kind), int(HIKE_EVENT_NONE));
    CHECK(is_key(p.next(true), HIKE_KEY_ESCAPE));
    CHECK(p.drained());
}

TEST(hike_input_escape_that_turns_out_to_be_a_sequence_is_not_an_escape_key) {
    // Same first byte as the test above, but the rest arrives in time. Nothing
    // may be reported for the ESC on its own.
    Parser p;
    p.feed("\x1b");
    CHECK_EQ(int(p.next(false).kind), int(HIKE_EVENT_NONE));
    p.feed("[A");
    CHECK(is_key(p.next(false), HIKE_KEY_UP));
    CHECK(p.drained());
}

TEST(hike_input_double_escape_yields_an_escape_and_reparses_the_rest) {
    Parser p;
    p.feed("\x1b\x1b[A");
    CHECK(is_key(p.next(), HIKE_KEY_ESCAPE));
    CHECK(is_key(p.next(), HIKE_KEY_UP));
    CHECK(p.drained());
}

// ============================================================== splits ====

TEST(hike_input_sequence_split_across_two_feeds) {
    Parser p;
    p.feed("\x1b[1;");
    CHECK_EQ(int(p.next().kind), int(HIKE_EVENT_NONE));
    p.feed("5A");
    CHECK(is_key(p.next(), HIKE_KEY_UP, HIKE_MOD_CTRL));
    CHECK(p.drained());
}

TEST(hike_input_sequence_split_one_byte_at_a_time) {
    Parser p;
    const std::string seq = csi("15;5~");
    for (size_t i = 0; i + 1 < seq.size(); ++i) {
        p.feed(std::string(1, seq[i]));
        CHECK_EQ(int(p.next().kind), int(HIKE_EVENT_NONE));
    }
    p.feed(std::string(1, seq.back()));
    CHECK(is_key(p.next(), HIKE_KEY_F5, HIKE_MOD_CTRL));
}

TEST(hike_input_a_flush_mid_sequence_does_not_split_the_sequence) {
    // The timeout can fire while a sequence is genuinely in flight. A partial
    // CSI is still unfinished, and must not decay into an Escape key plus
    // literal text.
    Parser p;
    p.feed("\x1b[1;");
    CHECK_EQ(int(p.next(true).kind), int(HIKE_EVENT_NONE));
    p.feed("5A");
    CHECK(is_key(p.next(), HIKE_KEY_UP, HIKE_MOD_CTRL));
}

TEST(hike_input_several_events_in_one_chunk) {
    Parser p;
    p.feed("a\x1b[Ab\r\x1b[3~");
    CHECK(is_char(p.next(), 'a'));
    CHECK(is_key(p.next(), HIKE_KEY_UP));
    CHECK(is_char(p.next(), 'b'));
    CHECK(is_key(p.next(), HIKE_KEY_ENTER));
    CHECK(is_key(p.next(), HIKE_KEY_DELETE));
    CHECK(p.drained());
}

// =============================================================== mouse ====

TEST(hike_input_sgr_mouse_press_converts_to_zero_based_cells) {
    Parser p;
    hike_event e = p.one(csi("<0;10;5M"));
    CHECK_EQ(int(e.kind), int(HIKE_EVENT_MOUSE));
    CHECK_EQ(int(e.mouse.kind), int(HIKE_MOUSE_PRESS));
    CHECK_EQ(e.mouse.button, 0);
    // The wire counts from 1 and this API counts from 0.
    CHECK_EQ(e.mouse.x, 9);
    CHECK_EQ(e.mouse.y, 4);
}

TEST(hike_input_sgr_mouse_top_left_cell_is_zero_zero) {
    Parser p;
    hike_event e = p.one(csi("<0;1;1M"));
    CHECK_EQ(e.mouse.x, 0);
    CHECK_EQ(e.mouse.y, 0);
}

TEST(hike_input_sgr_mouse_middle_and_right_buttons) {
    Parser p;
    CHECK_EQ(p.one(csi("<1;3;4M")).mouse.button, 1);
    CHECK_EQ(p.one(csi("<2;3;4M")).mouse.button, 2);
}

TEST(hike_input_sgr_mouse_release_is_the_lowercase_final) {
    Parser p;
    hike_event e = p.one(csi("<0;10;5m"));
    CHECK_EQ(int(e.kind), int(HIKE_EVENT_MOUSE));
    CHECK_EQ(int(e.mouse.kind), int(HIKE_MOUSE_RELEASE));
    CHECK_EQ(e.mouse.x, 9);
    CHECK_EQ(e.mouse.y, 4);
}

TEST(hike_input_sgr_mouse_motion) {
    Parser p;
    // 32 is the motion bit; 35 is motion with no button held.
    hike_event held = p.one(csi("<32;7;8M"));
    CHECK_EQ(int(held.mouse.kind), int(HIKE_MOUSE_MOVE));
    CHECK_EQ(held.mouse.x, 6);
    CHECK_EQ(held.mouse.y, 7);
    hike_event bare = p.one(csi("<35;2;2M"));
    CHECK_EQ(int(bare.mouse.kind), int(HIKE_MOUSE_MOVE));
    CHECK_EQ(bare.mouse.x, 1);
    CHECK_EQ(bare.mouse.y, 1);
}

TEST(hike_input_sgr_mouse_scroll) {
    Parser p;
    hike_event up = p.one(csi("<64;3;9M"));
    CHECK_EQ(int(up.mouse.kind), int(HIKE_MOUSE_SCROLL_UP));
    CHECK_EQ(up.mouse.x, 2);
    CHECK_EQ(up.mouse.y, 8);
    hike_event down = p.one(csi("<65;3;9M"));
    CHECK_EQ(int(down.mouse.kind), int(HIKE_MOUSE_SCROLL_DOWN));
}

TEST(hike_input_sgr_mouse_modifiers) {
    Parser p;
    // 4 shift, 8 alt, 16 ctrl, all on top of button 0.
    CHECK_EQ(int(p.one(csi("<4;1;1M")).mouse.mods), int(HIKE_MOD_SHIFT));
    CHECK_EQ(int(p.one(csi("<8;1;1M")).mouse.mods), int(HIKE_MOD_ALT));
    CHECK_EQ(int(p.one(csi("<16;1;1M")).mouse.mods), int(HIKE_MOD_CTRL));
    CHECK_EQ(int(p.one(csi("<28;1;1M")).mouse.mods),
             int(HIKE_MOD_SHIFT | HIKE_MOD_ALT | HIKE_MOD_CTRL));
}

TEST(hike_input_sgr_mouse_large_coordinates) {
    // The reason SGR exists: the older encoding could not express a column
    // past 223, and wide terminals are ordinary now.
    Parser p;
    hike_event e = p.one(csi("<0;400;300M"));
    CHECK_EQ(e.mouse.x, 399);
    CHECK_EQ(e.mouse.y, 299);
}

TEST(hike_input_sgr_mouse_split_across_feeds) {
    Parser p;
    p.feed("\x1b[<0;12");
    CHECK_EQ(int(p.next().kind), int(HIKE_EVENT_NONE));
    p.feed("3;45M");
    hike_event e = p.next();
    CHECK_EQ(int(e.kind), int(HIKE_EVENT_MOUSE));
    CHECK_EQ(e.mouse.x, 122);
    CHECK_EQ(e.mouse.y, 44);
}

// =============================================================== focus ====

TEST(hike_input_focus_in_and_out) {
    Parser p;
    hike_event in = p.one(csi("I"));
    CHECK_EQ(int(in.kind), int(HIKE_EVENT_FOCUS));
    CHECK(in.focused);
    hike_event out = p.one(csi("O"));
    CHECK_EQ(int(out.kind), int(HIKE_EVENT_FOCUS));
    CHECK(!out.focused);
}

// =============================================================== paste ====

TEST(hike_input_bracketed_paste_arrives_as_one_event) {
    Parser p;
    p.feed("\x1b[200~hello world\x1b[201~");
    hike_event e = p.next();
    CHECK_EQ(int(e.kind), int(HIKE_EVENT_PASTE));
    CHECK_EQ(int(e.paste_len), 11);
    CHECK(std::string(e.paste, e.paste_len) == "hello world");
    CHECK(p.drained());
}

TEST(hike_input_paste_body_is_never_interpreted) {
    // This is the entire point of bracketed paste. A pasted ESC must land in
    // the string as a byte, not become an Alt chord, and a pasted CSI must not
    // become an arrow key -- otherwise pasting a terminal transcript into an
    // editor would type commands into it.
    Parser p;
    const std::string body = "a\x1b" "b\x1b[Ac\rd";
    p.feed("\x1b[200~" + body + "\x1b[201~");
    hike_event e = p.next();
    CHECK_EQ(int(e.kind), int(HIKE_EVENT_PASTE));
    CHECK_EQ(int(e.paste_len), int(body.size()));
    CHECK(std::string(e.paste, e.paste_len) == body);
    CHECK(p.drained());
}

TEST(hike_input_paste_split_across_many_feeds) {
    Parser p;
    p.feed("\x1b[200~one ");
    CHECK_EQ(int(p.next().kind), int(HIKE_EVENT_NONE));
    p.feed("two ");
    CHECK_EQ(int(p.next().kind), int(HIKE_EVENT_NONE));
    p.feed("three");
    CHECK_EQ(int(p.next().kind), int(HIKE_EVENT_NONE));
    p.feed("\x1b[201~");
    hike_event e = p.next();
    CHECK_EQ(int(e.kind), int(HIKE_EVENT_PASTE));
    CHECK(std::string(e.paste, e.paste_len) == "one two three");
}

TEST(hike_input_paste_end_marker_split_down_the_middle) {
    // The marker is six bytes and can be cut anywhere. If the parser flushed
    // its buffer greedily it would swallow half the marker into the body and
    // then never find the end.
    Parser p;
    p.feed("\x1b[200~body\x1b[20");
    CHECK_EQ(int(p.next().kind), int(HIKE_EVENT_NONE));
    p.feed("1~");
    hike_event e = p.next();
    CHECK_EQ(int(e.kind), int(HIKE_EVENT_PASTE));
    CHECK(std::string(e.paste, e.paste_len) == "body");
}

TEST(hike_input_paste_does_not_swallow_the_keys_after_it) {
    Parser p;
    p.feed("\x1b[200~x\x1b[201~\x1b[Ay");
    CHECK_EQ(int(p.next().kind), int(HIKE_EVENT_PASTE));
    CHECK(is_key(p.next(), HIKE_KEY_UP));
    CHECK(is_char(p.next(), 'y'));
    CHECK(p.drained());
}

TEST(hike_input_empty_paste) {
    Parser p;
    hike_event e = p.one("\x1b[200~\x1b[201~");
    CHECK_EQ(int(e.kind), int(HIKE_EVENT_PASTE));
    CHECK_EQ(int(e.paste_len), 0);
}

TEST(hike_input_stray_paste_end_marker_is_ignored) {
    Parser p;
    p.feed("\x1b[201~a");
    CHECK(is_char(p.next(), 'a'));
}

// ========================================================= malformed ====

TEST(hike_input_unknown_csi_is_discarded_and_the_next_key_still_parses) {
    // Terminals send sequences no library knows -- device attribute replies,
    // vendor extensions. Reporting one as the wrong key is worse than silence,
    // and leaving it half-consumed would corrupt everything after it.
    Parser p;
    p.feed("\x1b[?1;2c\x1b[A");
    CHECK(is_key(p.next(), HIKE_KEY_UP));
    CHECK(p.drained());
}

TEST(hike_input_csi_with_an_illegal_byte_resynchronises) {
    Parser p;
    // A CSI interrupted by a byte that cannot appear in one. The parser has to
    // cut its losses at the bad byte rather than keep consuming.
    p.feed("\x1b[1;\x01\x1b[B");
    hike_event first = p.next();
    // Whatever it makes of the wreckage, the arrow key after it must arrive.
    while (first.kind != HIKE_EVENT_NONE &&
           !(first.kind == HIKE_EVENT_KEY && first.key.key == HIKE_KEY_DOWN))
        first = p.next();
    CHECK(is_key(first, HIKE_KEY_DOWN));
}

TEST(hike_input_absurdly_long_csi_does_not_wedge_the_parser) {
    // A final byte that never comes must not leave the parser waiting forever,
    // because from then on it would answer every real keystroke with silence.
    Parser p;
    p.feed("\x1b[" + std::string(200, '1'));
    CHECK_EQ(int(p.next().kind), int(HIKE_EVENT_NONE));
    p.feed("\x1b[A");
    hike_event e = p.next();
    while (e.kind != HIKE_EVENT_NONE && !is_key(e, HIKE_KEY_UP)) e = p.next();
    CHECK(is_key(e, HIKE_KEY_UP));
}

TEST(hike_input_unknown_ss3_final_is_discarded) {
    Parser p;
    p.feed("\x1bOZ" "q");
    CHECK(is_char(p.next(), 'q'));
    CHECK(p.drained());
}

TEST(hike_input_csi_with_no_parameters_where_some_are_required) {
    Parser p;
    // A tilde form with no number, and an SGR mouse report missing its
    // coordinates. Both are meaningless; both must vanish quietly.
    p.feed("\x1b[~\x1b[<0M" "z");
    CHECK(is_char(p.next(), 'z'));
    CHECK(p.drained());
}

TEST(hike_input_events_survive_a_burst_of_garbage_between_them) {
    Parser p;
    p.feed("\x1b[A\x1b[?25h\x1b[B\x1b[999~\x1b[C");
    CHECK(is_key(p.next(), HIKE_KEY_UP));
    CHECK(is_key(p.next(), HIKE_KEY_DOWN));
    CHECK(is_key(p.next(), HIKE_KEY_RIGHT));
    CHECK(p.drained());
}
