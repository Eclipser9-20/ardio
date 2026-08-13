/* Terminal input: turning a byte stream back into keys, mouse reports, pastes
 * and focus changes.
 *
 * A terminal delivers everything down one channel. A keystroke, a mouse click
 * and the middle of a pasted paragraph are all just bytes on the same
 * descriptor, distinguished only by escape sequences, and those bytes arrive
 * in whatever chunks the kernel felt like handing over. A read can stop in the
 * middle of "ESC [ 1 ; 5 A" or between the two halves of a UTF-8 character.
 *
 * So the parser here is a state machine over a buffer rather than a function
 * that assumes it can see a whole sequence. Bytes go in; complete events come
 * out; anything incomplete stays in the buffer until the rest of it shows up.
 * Nothing is ever dropped on a chunk boundary, because a chunk boundary is not
 * a thing the parser can observe.
 *
 * The other rule is that the parser must never get stuck. A sequence libhike
 * does not know -- and terminals emit plenty, from device attribute replies to
 * vendor extensions -- has to be consumed and discarded, not reported as some
 * wrong key and not left half-eaten at the front of the buffer. A TUI that
 * responds to one bad byte by misreading everything after it is the failure
 * mode users describe as "the keyboard stopped working". */

#include "hike_input_internal.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <errno.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/ioctl.h>
#  include <time.h>
#  include <unistd.h>
#endif

/* ------------------------------------------------------------- buffers --- */

static bool reserve(unsigned char** buf, size_t* cap, size_t need) {
    if (*cap >= need) return true;
    size_t want = *cap ? *cap : 64;
    while (want < need) want *= 2;
    unsigned char* grown = (unsigned char*)realloc(*buf, want);
    if (!grown) return false;
    *buf = grown;
    *cap = want;
    return true;
}

void hike_input_init(hike_input_parser* p) {
    memset(p, 0, sizeof(*p));
}

void hike_input_free(hike_input_parser* p) {
    free(p->buf);
    free(p->paste);
    memset(p, 0, sizeof(*p));
}

void hike_input_feed(hike_input_parser* p, const void* bytes, size_t n) {
    if (n == 0) return;
    /* Out of memory here can only be handled by dropping the input, since the
     * caller is told nothing but "no event yet". That is survivable -- the
     * parser stays synchronised on whatever it did keep -- and it is the only
     * option a void-returning feed has. */
    if (!reserve(&p->buf, &p->cap, p->len + n)) return;
    memcpy(p->buf + p->len, bytes, n);
    p->len += n;
}

static void consume(hike_input_parser* p, size_t n) {
    if (n >= p->len) { p->len = 0; return; }
    memmove(p->buf, p->buf + n, p->len - n);
    p->len -= n;
}

static bool paste_append(hike_input_parser* p, const unsigned char* b, size_t n) {
    unsigned char* as_bytes = (unsigned char*)p->paste;
    if (!reserve(&as_bytes, &p->paste_cap, p->paste_len + n + 1)) return false;
    p->paste = (char*)as_bytes;
    memcpy(p->paste + p->paste_len, b, n);
    p->paste_len += n;
    p->paste[p->paste_len] = '\0';
    return true;
}

/* ---------------------------------------------------------------- UTF-8 --- */

/* libhike has a UTF-8 decoder in its public surface, but the parser keeps its
 * own, because this one has a third answer the public one cannot express: the
 * input is not invalid, it is merely not finished yet. Telling "this is a
 * two-byte character whose second byte has not arrived" apart from "this is
 * garbage" is the whole reason a character can be split across two reads and
 * still come out right. */
enum { UTF8_INCOMPLETE = -1, UTF8_INVALID = -2 };

static int utf8_decode(const unsigned char* b, size_t n, uint32_t* out) {
    unsigned char c = b[0];
    int need;
    uint32_t cp;
    if (c < 0x80) { *out = c; return 1; }
    else if ((c & 0xE0) == 0xC0) { need = 2; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { need = 3; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { need = 4; cp = c & 0x07u; }
    else return UTF8_INVALID;   /* a continuation byte or 0xF8+ cannot lead */

    if ((size_t)need > n) return UTF8_INCOMPLETE;
    for (int i = 1; i < need; ++i) {
        if ((b[i] & 0xC0) != 0x80) return UTF8_INVALID;
        cp = (cp << 6) | (uint32_t)(b[i] & 0x3F);
    }
    /* Overlong forms and surrogates are rejected rather than passed through:
     * they are the classic way a decoder is talked into producing a code point
     * the encoder never wrote. */
    if ((need == 2 && cp < 0x80) || (need == 3 && cp < 0x800) ||
        (need == 4 && cp < 0x10000))
        return UTF8_INVALID;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return UTF8_INVALID;
    *out = cp;
    return need;
}

/* ----------------------------------------------------------- key helpers --- */

static void key_event(hike_event* out, hike_key key, uint32_t ch, uint8_t mods) {
    memset(out, 0, sizeof(*out));
    out->kind = HIKE_EVENT_KEY;
    out->key.key = key;
    out->key.ch = ch;
    out->key.mods = mods;
}

/* xterm reports modifiers as a 1-based bitmask, so the plain key is 1 and
 * Ctrl alone is 5. Anything below 1 means the terminal sent no modifier
 * parameter at all. */
static uint8_t decode_mods(int param) {
    uint8_t mods = 0;
    if (param < 2) return 0;
    int bits = param - 1;
    if (bits & 1) mods |= HIKE_MOD_SHIFT;
    if (bits & 2) mods |= HIKE_MOD_ALT;
    if (bits & 4) mods |= HIKE_MOD_CTRL;
    return mods;
}

/* The letter finals shared by CSI and SS3 forms. A terminal in application
 * cursor mode sends ESC O A for Up and ESC [ A otherwise; the difference is a
 * mode the application may not have chosen, so both mean the same key here. */
static bool letter_key(unsigned char final, hike_key* out) {
    switch (final) {
    case 'A': *out = HIKE_KEY_UP; return true;
    case 'B': *out = HIKE_KEY_DOWN; return true;
    case 'C': *out = HIKE_KEY_RIGHT; return true;
    case 'D': *out = HIKE_KEY_LEFT; return true;
    case 'H': *out = HIKE_KEY_HOME; return true;
    case 'F': *out = HIKE_KEY_END; return true;
    case 'P': *out = HIKE_KEY_F1; return true;
    case 'Q': *out = HIKE_KEY_F2; return true;
    case 'R': *out = HIKE_KEY_F3; return true;
    case 'S': *out = HIKE_KEY_F4; return true;
    default: return false;
    }
}

/* The numeric finals, "CSI n ~". The gaps are real: there is no 16, and F5 is
 * 15 rather than 16, because the numbering grew out of the DEC VT220 keypad
 * and was never tidy. */
static bool tilde_key(int n, hike_key* out) {
    switch (n) {
    case 1: case 7: *out = HIKE_KEY_HOME; return true;
    case 2: *out = HIKE_KEY_INSERT; return true;
    case 3: *out = HIKE_KEY_DELETE; return true;
    case 4: case 8: *out = HIKE_KEY_END; return true;
    case 5: *out = HIKE_KEY_PAGE_UP; return true;
    case 6: *out = HIKE_KEY_PAGE_DOWN; return true;
    case 11: *out = HIKE_KEY_F1; return true;
    case 12: *out = HIKE_KEY_F2; return true;
    case 13: *out = HIKE_KEY_F3; return true;
    case 14: *out = HIKE_KEY_F4; return true;
    case 15: *out = HIKE_KEY_F5; return true;
    case 17: *out = HIKE_KEY_F6; return true;
    case 18: *out = HIKE_KEY_F7; return true;
    case 19: *out = HIKE_KEY_F8; return true;
    case 20: *out = HIKE_KEY_F9; return true;
    case 21: *out = HIKE_KEY_F10; return true;
    case 23: *out = HIKE_KEY_F11; return true;
    case 24: *out = HIKE_KEY_F12; return true;
    default: return false;
    }
}

/* ------------------------------------------------------------ parse step --- */

/* What one attempt at the front of the buffer produced. */
enum { PARSE_EVENT = 1, PARSE_NEED_MORE = 0, PARSE_DISCARD = -1 };

/* One byte outside an escape sequence: text, or one of the C0 controls that a
 * keyboard produces. */
static int parse_plain(const unsigned char* b, size_t n, hike_event* out,
                       size_t* used) {
    unsigned char c = b[0];
    *used = 1;

    switch (c) {
    case 0x0D: case 0x0A: key_event(out, HIKE_KEY_ENTER, 0, 0); return PARSE_EVENT;
    case 0x09: key_event(out, HIKE_KEY_TAB, 0, 0); return PARSE_EVENT;
    /* Both bytes mean Backspace, and which one arrives is a property of the
     * terminal rather than of the key: 0x7F (DEL) from most Unix terminals,
     * 0x08 (BS) from the Windows console and from terminals configured the
     * older way. A library that honoured only one of them would appear broken
     * on half the machines it runs on. */
    case 0x08: case 0x7F: key_event(out, HIKE_KEY_BACKSPACE, 0, 0); return PARSE_EVENT;
    default: break;
    }

    /* Ctrl+letter arrives as the letter with the top three bits cleared; the
     * terminal has already done the folding, and there is no way to recover
     * the original case, so the letter is reported lowercase. */
    if (c >= 0x01 && c <= 0x1A) {
        key_event(out, HIKE_KEY_CHAR, (uint32_t)('a' + c - 1), HIKE_MOD_CTRL);
        return PARSE_EVENT;
    }
    if (c == 0x00) {   /* Ctrl+Space, by the same folding rule */
        key_event(out, HIKE_KEY_CHAR, (uint32_t)' ', HIKE_MOD_CTRL);
        return PARSE_EVENT;
    }
    if (c >= 0x1C && c <= 0x1F) {
        static const char punct[] = { '\\', ']', '^', '_' };
        key_event(out, HIKE_KEY_CHAR, (uint32_t)punct[c - 0x1C], HIKE_MOD_CTRL);
        return PARSE_EVENT;
    }

    {
        uint32_t cp = 0;
        int len = utf8_decode(b, n, &cp);
        if (len == UTF8_INCOMPLETE) {
            /* Four bytes is the longest legal encoding, so anything still
             * incomplete at that length is not going to be completed by
             * waiting -- drop the lead byte and resynchronise. */
            if (n >= 4) { *used = 1; return PARSE_DISCARD; }
            return PARSE_NEED_MORE;
        }
        if (len == UTF8_INVALID) { *used = 1; return PARSE_DISCARD; }
        *used = (size_t)len;
        key_event(out, HIKE_KEY_CHAR, cp, 0);
        return PARSE_EVENT;
    }
}

/* CSI: ESC [ , then parameter bytes, then optional intermediates, then one
 * final byte in 0x40-0x7E that says what the sequence was. */
static int parse_csi(const unsigned char* b, size_t n, hike_event* out,
                     hike_input_parser* p, size_t* used) {
    size_t i = 2;                 /* past ESC and '[' */
    unsigned char private_marker = 0;
    if (i < n && b[i] >= 0x3C && b[i] <= 0x3F) private_marker = b[i++];

    int params[8];
    int nparams = 0;
    bool seen_digit = false;
    int value = 0;

    for (;;) {
        if (i >= n) {
            /* A sequence this long is not one we are going to recognise, and
             * waiting for a final byte that is never coming would wedge the
             * parser forever. Bound it and give up. */
            if (n - 2 > 64) { *used = n; return PARSE_DISCARD; }
            return PARSE_NEED_MORE;
        }
        unsigned char c = b[i];
        if (c >= '0' && c <= '9') {
            seen_digit = true;
            if (value < 100000) value = value * 10 + (c - '0');
            ++i;
        } else if (c == ';' || c == ':') {
            if (nparams < 8) params[nparams++] = seen_digit ? value : 0;
            value = 0;
            seen_digit = false;
            ++i;
        } else if (c >= 0x20 && c <= 0x2F) {
            ++i;                  /* intermediate byte, ignored */
        } else if (c >= 0x40 && c <= 0x7E) {
            if (seen_digit && nparams < 8) params[nparams++] = value;
            ++i;
            break;
        } else {
            /* Anything else cannot appear in a CSI sequence. The terminal and
             * this parser disagree about where the sequence ended, so discard
             * what we have rather than guess. */
            *used = i;
            return PARSE_DISCARD;
        }
    }

    *used = i;
    unsigned char final = b[i - 1];

    /* SGR mouse: CSI < b ; x ; y M for a press or motion, m for a release.
     * This form is used in preference to the original X10 encoding because
     * X10 packs coordinates into single bytes and so cannot address a column
     * past 223 -- on any normal modern window, the right-hand side of the
     * screen would report nonsense. */
    if (private_marker == '<' && (final == 'M' || final == 'm')) {
        if (nparams < 3) return PARSE_DISCARD;
        int flags = params[0], x = params[1], y = params[2];
        memset(out, 0, sizeof(*out));
        out->kind = HIKE_EVENT_MOUSE;
        /* The wire is 1-based because it descends from cursor addressing; the
         * grid this library exposes is 0-based. Converting here means no
         * caller ever has to remember which convention it is holding. */
        out->mouse.x = x > 0 ? x - 1 : 0;
        out->mouse.y = y > 0 ? y - 1 : 0;
        out->mouse.button = flags & 3;
        if (flags & 4) out->mouse.mods |= HIKE_MOD_SHIFT;
        if (flags & 8) out->mouse.mods |= HIKE_MOD_ALT;
        if (flags & 16) out->mouse.mods |= HIKE_MOD_CTRL;

        if (flags & 64) {
            /* Wheel events reuse the button field: 64 is up, 65 is down. */
            out->mouse.kind = (flags & 1) ? HIKE_MOUSE_SCROLL_DOWN
                                          : HIKE_MOUSE_SCROLL_UP;
            out->mouse.button = 0;
        } else if (final == 'm') {
            out->mouse.kind = HIKE_MOUSE_RELEASE;
        } else if (flags & 32) {
            out->mouse.kind = HIKE_MOUSE_MOVE;
            /* Motion with no button held is reported as button 3, which is not
             * a button this API has. Report it as the left button's index but
             * with the MOVE kind, which is what a caller reads anyway. */
            if ((flags & 3) == 3) out->mouse.button = 0;
        } else {
            out->mouse.kind = HIKE_MOUSE_PRESS;
        }
        return PARSE_EVENT;
    }

    if (private_marker != 0) return PARSE_DISCARD;  /* some other private mode */

    /* Focus tracking, which has no parameters at all. */
    if (final == 'I' && nparams == 0) {
        memset(out, 0, sizeof(*out));
        out->kind = HIKE_EVENT_FOCUS;
        out->focused = true;
        return PARSE_EVENT;
    }
    if (final == 'O' && nparams == 0) {
        memset(out, 0, sizeof(*out));
        out->kind = HIKE_EVENT_FOCUS;
        out->focused = false;
        return PARSE_EVENT;
    }

    if (final == '~') {
        if (nparams < 1) return PARSE_DISCARD;
        if (params[0] == 200) { p->in_paste = true; p->paste_len = 0; return PARSE_DISCARD; }
        if (params[0] == 201) return PARSE_DISCARD;  /* an end with no start */
        hike_key key;
        if (!tilde_key(params[0], &key)) return PARSE_DISCARD;
        key_event(out, key, 0, nparams >= 2 ? decode_mods(params[1]) : 0);
        return PARSE_EVENT;
    }

    {
        hike_key key;
        if (!letter_key(final, &key)) return PARSE_DISCARD;
        /* The modifier form is CSI 1 ; 5 A: the first parameter is a count
         * that is always 1 for these keys, and the modifier is the second. */
        key_event(out, key, 0, nparams >= 2 ? decode_mods(params[1]) : 0);
        return PARSE_EVENT;
    }
}

static int parse_one(hike_input_parser* p, hike_event* out, bool flush,
                     size_t* used);

/* ESC O x -- the SS3 form, used for the cursor and function keys when the
 * terminal is in application keypad mode. */
static int parse_ss3(const unsigned char* b, size_t n, hike_event* out,
                     size_t* used) {
    if (n < 3) return PARSE_NEED_MORE;
    *used = 3;
    /* Some terminals send the modifier through SS3 as ESC O 1 ; 5 A. Treat a
     * digit after the O as the start of a parameterised form and re-read it
     * with the CSI machinery, which already knows that shape. */
    if (b[2] >= '0' && b[2] <= '9') {
        unsigned char rewritten[80];
        size_t take = n < sizeof(rewritten) ? n : sizeof(rewritten);
        memcpy(rewritten, b, take);
        rewritten[1] = '[';
        hike_input_parser scratch;
        memset(&scratch, 0, sizeof(scratch));
        return parse_csi(rewritten, take, out, &scratch, used);
    }
    hike_key key;
    if (!letter_key(b[2], &key)) return PARSE_DISCARD;
    key_event(out, key, 0, 0);
    return PARSE_EVENT;
}

/* Everything that starts with ESC. */
static int parse_escape(hike_input_parser* p, hike_event* out, bool flush,
                        size_t* used) {
    const unsigned char* b = p->buf;
    size_t n = p->len;

    /* This is the genuinely ambiguous case in terminal input, and there is no
     * fully correct answer to it. The Escape key sends 0x1B, and so does the
     * first byte of every arrow key, function key and mouse report. At the
     * moment a lone 0x1B is in the buffer, the two are indistinguishable --
     * the information that would separate them has not been transmitted yet.
     *
     * The usual resolution, and the one taken here, is a timeout: wait
     * HIKE_ESC_TIMEOUT_MS for a following byte, and if none arrives, call it
     * the Escape key. That is what `flush` means -- the caller waited and
     * nothing came.
     *
     * The cost is real and worth stating. Pressing Escape adds that delay
     * before the application reacts, so a vi-style modal UI feels very
     * slightly sticky on the one key it uses most. Shortening the timeout
     * trades that back for a worse failure: over ssh on a slow link, an arrow
     * key's bytes can be split by more than the timeout, and the tail of the
     * sequence would then be misread as literal text appearing in the user's
     * document. Losing a keystroke's latency is recoverable; corrupting input
     * is not, so the timeout is set long enough to cover a bad connection. */
    if (n == 1) {
        if (!flush) return PARSE_NEED_MORE;
        *used = 1;
        key_event(out, HIKE_KEY_ESCAPE, 0, 0);
        return PARSE_EVENT;
    }

    if (b[1] == '[') return parse_csi(b, n, out, p, used);
    if (b[1] == 'O') return parse_ss3(b, n, out, used);

    /* ESC followed by ESC. The second one might begin a sequence of its own,
     * so only the first is resolved here and the rest is re-examined from the
     * top on the next pass. */
    if (b[1] == 0x1B) {
        *used = 1;
        key_event(out, HIKE_KEY_ESCAPE, 0, 0);
        return PARSE_EVENT;
    }

    /* Any other byte after ESC is Alt: terminals encode Alt+X by sending ESC
     * and then exactly what X would have sent on its own. So parse the tail as
     * if the ESC were not there and add the modifier to whatever comes out. */
    {
        size_t inner = 0;
        int r = parse_plain(b + 1, n - 1, out, &inner);
        if (r == PARSE_NEED_MORE) return PARSE_NEED_MORE;   /* split character */
        *used = inner + 1;
        if (r != PARSE_EVENT) return PARSE_DISCARD;
        out->key.mods |= HIKE_MOD_ALT;
        return PARSE_EVENT;
    }
}

/* Inside a bracketed paste, nothing is interpreted.
 *
 * That is the entire purpose of bracketed paste: the terminal promises that
 * everything between the markers is literal text, so a pasted ESC stays an ESC
 * character in the pasted string instead of becoming an Alt chord, and pasted
 * newlines do not each look like the user pressing Enter. An editor without
 * this pastes a snippet and watches its auto-indent destroy it. */
static int parse_paste(hike_input_parser* p, hike_event* out, size_t* used) {
    static const unsigned char kEnd[] = "\x1b[201~";
    const size_t end_len = sizeof(kEnd) - 1;

    for (size_t i = 0; i + end_len <= p->len; ++i) {
        if (memcmp(p->buf + i, kEnd, end_len) != 0) continue;
        if (!paste_append(p, p->buf, i)) { *used = i + end_len; p->in_paste = false; return PARSE_DISCARD; }
        p->in_paste = false;
        *used = i + end_len;
        memset(out, 0, sizeof(*out));
        out->kind = HIKE_EVENT_PASTE;
        out->paste = p->paste ? p->paste : "";
        out->paste_len = p->paste_len;
        return PARSE_EVENT;
    }

    /* No terminator yet. Everything except a possible split prefix of one can
     * be moved into the paste body now, which keeps the input buffer from
     * growing to the size of the whole paste. */
    size_t keep = p->len < end_len - 1 ? p->len : end_len - 1;
    size_t take = p->len - keep;
    if (take) {
        if (!paste_append(p, p->buf, take)) { p->in_paste = false; }
        consume(p, take);
    }
    return PARSE_NEED_MORE;
}

static int parse_one(hike_input_parser* p, hike_event* out, bool flush,
                     size_t* used) {
    *used = 0;
    if (p->in_paste) return parse_paste(p, out, used);
    if (p->buf[0] == 0x1B) return parse_escape(p, out, flush, used);
    return parse_plain(p->buf, p->len, out, used);
}

bool hike_input_next(hike_input_parser* p, hike_event* out, bool flush) {
    for (;;) {
        if (p->len == 0) return false;
        size_t used = 0;
        int r = parse_one(p, out, flush, &used);
        if (r == PARSE_NEED_MORE) return false;
        /* A parse step that recognised nothing must still make progress, or
         * the loop below spins on a byte it will never accept. */
        if (used == 0) used = 1;
        consume(p, used);
        if (r == PARSE_EVENT) return true;
    }
}

/* =========================================================== hike_poll === */

/* The parser state belongs to the process, not to a hike_context, because the
 * terminal it reads belongs to the process too: there is one standard input,
 * and two contexts consuming it would each see half of every sequence. Keeping
 * it here also means this file needs nothing from the context's private
 * layout, so the input code and the terminal code stay independent. */
static hike_input_parser g_parser;
static bool g_parser_ready;

#if !defined(_WIN32)

/* SIGWINCH tells us the window changed size. Almost nothing is legal in a
 * signal handler -- not malloc, not the parser, not even reading the new size,
 * since ioctl is only async-signal-safe by convention and the handler may
 * interrupt the poll loop at any point. So the handler does the one thing the
 * standard actually guarantees: it stores a flag in a volatile sig_atomic_t.
 * All the real work happens back in hike_poll, on the ordinary call stack,
 * where ioctl and allocation are fine. */
static volatile sig_atomic_t g_resized;

static void on_sigwinch(int sig) {
    (void)sig;
    g_resized = 1;
}

static void install_sigwinch(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigwinch;
    /* SA_RESTART deliberately left off: the point of this handler is to make
     * the blocking poll() return early so a resize is reported promptly, and
     * restarting the call would defeat that. EINTR is handled below. */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);
}

static bool fill_resize(hike_event* out) {
    struct winsize ws;
    memset(out, 0, sizeof(*out));
    out->kind = HIKE_EVENT_RESIZE;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 ||
        ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        out->size.w = ws.ws_col;
        out->size.h = ws.ws_row;
    } else {
        /* A terminal that will not report its size is still a terminal. The
         * conventional 80x24 is a better answer than zero, which would make
         * every layout collapse. */
        out->size.w = 80;
        out->size.h = 24;
    }
    return true;
}

/* Milliseconds since an arbitrary fixed point, for deadline arithmetic. */
static long long now_ms(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool hike_poll(hike_context* ctx, hike_event* out, int timeout_ms) {
    (void)ctx;
    if (!out) return false;
    if (!g_parser_ready) {
        hike_input_init(&g_parser);
        install_sigwinch();
        g_parser_ready = true;
    }

    long long deadline = timeout_ms >= 0 ? now_ms() + timeout_ms : 0;

    for (;;) {
        if (g_resized) {
            g_resized = 0;
            return fill_resize(out);
        }
        /* Anything already buffered is answered without touching the
         * descriptor, so a read that delivered three keystrokes at once
         * produces three events rather than one. */
        if (hike_input_next(&g_parser, out, false)) return true;

        int wait_ms;
        bool esc_pending = g_parser.len > 0 && g_parser.buf[0] == 0x1B;
        if (timeout_ms < 0) {
            wait_ms = esc_pending ? HIKE_ESC_TIMEOUT_MS : -1;
        } else {
            long long remaining = deadline - now_ms();
            if (remaining < 0) remaining = 0;
            if (esc_pending && remaining > HIKE_ESC_TIMEOUT_MS)
                remaining = HIKE_ESC_TIMEOUT_MS;
            wait_ms = (int)remaining;
        }

        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r = poll(&pfd, 1, wait_ms);

        if (r < 0) {
            if (errno == EINTR) continue;   /* very likely our own SIGWINCH */
            return false;
        }
        if (r == 0) {
            /* Nothing arrived in time. If a bare ESC was waiting, this is the
             * moment it stops being ambiguous. */
            if (esc_pending && hike_input_next(&g_parser, out, true)) return true;
            if (timeout_ms >= 0 && now_ms() >= deadline) return false;
            continue;
        }

        unsigned char chunk[1024];
        ssize_t got = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (got > 0) {
            hike_input_feed(&g_parser, chunk, (size_t)got);
        } else if (got == 0) {
            return false;                    /* input closed */
        } else if (errno != EINTR && errno != EAGAIN) {
            return false;
        }
    }
}

#else /* _WIN32 */

/* UNTESTED. This path has never been run: there is no Windows machine in this
 * project's build or test setup, so what follows is written from the console
 * API's documented behaviour and reviewed by reading, not by executing. It is
 * included rather than omitted so a Windows port starts from something
 * concrete, but it should be treated as a first draft until someone runs it.
 *
 * The shape differs from POSIX in two ways. There is no SIGWINCH, so a resize
 * arrives as a WINDOW_BUFFER_SIZE_EVENT in the console input queue, which is
 * actually easier -- no signal handler, no async-signal-safety question at
 * all. And the console must have been put into VT input mode by the terminal
 * setup code (ENABLE_VIRTUAL_TERMINAL_INPUT), or key presses arrive as
 * structured records instead of the escape sequences this parser expects. */
bool hike_poll(hike_context* ctx, hike_event* out, int timeout_ms) {
    (void)ctx;
    if (!out) return false;
    if (!g_parser_ready) {
        hike_input_init(&g_parser);
        g_parser_ready = true;
    }

    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in == INVALID_HANDLE_VALUE || in == NULL) return false;

    DWORD deadline = GetTickCount() + (timeout_ms > 0 ? (DWORD)timeout_ms : 0);

    for (;;) {
        if (hike_input_next(&g_parser, out, false)) return true;

        bool esc_pending = g_parser.len > 0 && g_parser.buf[0] == 0x1B;
        DWORD wait;
        if (timeout_ms < 0) {
            wait = esc_pending ? (DWORD)HIKE_ESC_TIMEOUT_MS : INFINITE;
        } else {
            DWORD now = GetTickCount();
            wait = (deadline > now) ? (deadline - now) : 0;
            if (esc_pending && wait > (DWORD)HIKE_ESC_TIMEOUT_MS)
                wait = (DWORD)HIKE_ESC_TIMEOUT_MS;
        }

        DWORD r = WaitForSingleObject(in, wait);
        if (r == WAIT_TIMEOUT) {
            if (esc_pending && hike_input_next(&g_parser, out, true)) return true;
            if (timeout_ms >= 0) return false;
            continue;
        }
        if (r != WAIT_OBJECT_0) return false;

        /* The handle is signalled for any record, including ones that carry no
         * bytes, so peek first: calling ReadConsole on a queue holding only a
         * focus or resize record would block. */
        INPUT_RECORD recs[64];
        DWORD count = 0;
        if (!PeekConsoleInputW(in, recs, 64, &count) || count == 0) continue;

        bool has_key = false;
        for (DWORD i = 0; i < count; ++i) {
            if (recs[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
                /* Drain up to and including the resize record so it is not
                 * seen again, then report the size it carries. */
                DWORD read_back = 0;
                ReadConsoleInputW(in, recs, i + 1, &read_back);
                memset(out, 0, sizeof(*out));
                out->kind = HIKE_EVENT_RESIZE;
                out->size.w = recs[i].Event.WindowBufferSizeEvent.dwSize.X;
                out->size.h = recs[i].Event.WindowBufferSizeEvent.dwSize.Y;
                return true;
            }
            if (recs[i].EventType == KEY_EVENT) has_key = true;
        }
        if (!has_key) {
            DWORD dropped = 0;
            ReadConsoleInputW(in, recs, count, &dropped);
            continue;
        }

        /* With VT input enabled the key records carry the escape sequence's
         * bytes, and ReadFile hands them over as the byte stream the parser
         * wants, so the records themselves never need decoding here. */
        char chunk[1024];
        DWORD got = 0;
        if (!ReadFile(in, chunk, sizeof(chunk), &got, NULL)) return false;
        if (got == 0) return false;
        hike_input_feed(&g_parser, chunk, (size_t)got);
    }
}

#endif
