/* The terminal itself: raw mode, teardown, colour depth, and the diff that
 * turns two cell grids into the smallest sensible stream of bytes.
 *
 * Three things in this file are worth reading before the code.
 *
 * Restoring the terminal is not a nicety. Raw mode is a property of the
 * terminal device, not of the process, so a program that dies with raw mode on
 * hands its user a shell with no echo and no line editing -- a state most
 * people fix by closing the window. libhike therefore installs handlers for
 * the fatal signals it can catch, restores the terminal from them, and then
 * re-raises the signal with the default disposition so the process still dies
 * the way it was going to and still produces a core file if it was going to.
 * What this cannot cover is written down at the handler.
 *
 * Colour is reduced here rather than at the call site. A UI written against
 * truecolour has to stay legible on a 16-colour terminal, and the only place
 * that knows what the terminal in front of us supports is this file.
 *
 * The diff is the reason the library exists in this shape. A terminal is a
 * slow serial device: the cost of a frame is very close to the number of bytes
 * written, so present emits only cells that differ, batches adjacent ones into
 * runs to avoid a cursor move per cell, and changes SGR state only when the
 * style actually changes. A frame in which nothing changed writes nothing at
 * all, which is what makes it reasonable to present on every event.
 */

#include "hike_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------- misc --- */

const char* hike_status_text(hike_status status) {
    switch (status) {
        case HIKE_OK: return "ok";
        case HIKE_ERR_NOT_A_TERMINAL:
            return "standard output is not a terminal";
        case HIKE_ERR_TERMINAL_SETUP:
            return "the terminal could not be put into raw mode";
        case HIKE_ERR_OUT_OF_MEMORY: return "out of memory";
        case HIKE_ERR_UNSUPPORTED_PLATFORM:
            return "this platform is not supported";
        case HIKE_ERR_INVALID_ARGUMENT: return "invalid argument";
    }
    return "unknown error";
}

hike_options hike_default_options(void) {
    hike_options o;
    /* Mouse, paste and focus reporting are off by default because each one
     * makes the terminal send bytes a program that did not ask for them will
     * not understand. The alternate screen and a hidden cursor are on because
     * they are what a full-screen UI almost always wants, and both are undone
     * on shutdown. */
    o.mouse = false;
    o.bracketed_paste = false;
    o.focus_events = false;
    o.alternate_screen = true;
    o.hide_cursor = true;
    return o;
}

int hike_width(const hike_context* ctx) { return ctx ? ctx->w : 0; }
int hike_height(const hike_context* ctx) { return ctx ? ctx->h : 0; }
hike_color_depth hike_depth(const hike_context* ctx) {
    return ctx ? ctx->depth : HIKE_COLOR_NONE;
}

/* -------------------------------------------------------------- output --- */

void hike_out(hike_context* ctx, const char* bytes, size_t len) {
    if (ctx == NULL || bytes == NULL || len == 0) return;

    if (ctx->out_fd < 0) {
        if (ctx->sink_len + len + 1 > ctx->sink_cap) {
            size_t cap = ctx->sink_cap ? ctx->sink_cap : 256;
            while (cap < ctx->sink_len + len + 1) cap *= 2;
            char* grown = (char*)realloc(ctx->sink, cap);
            /* Dropping the bytes is the only option left, and a test sink that
             * silently lost output would be worse than one that shows short
             * output, so the length is left where it was rather than advanced
             * over bytes that were never stored. */
            if (grown == NULL) return;
            ctx->sink = grown;
            ctx->sink_cap = cap;
        }
        memcpy(ctx->sink + ctx->sink_len, bytes, len);
        ctx->sink_len += len;
        ctx->sink[ctx->sink_len] = '\0';
        return;
    }

#ifdef _WIN32
    (void)_write(ctx->out_fd, bytes, (unsigned)len);
#else
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(ctx->out_fd, bytes + done, len - done);
        if (n > 0) { done += (size_t)n; continue; }
        /* A signal can interrupt a write; anything else means the terminal is
         * gone and there is nothing useful left to do about it. Half an escape
         * sequence on the wire would corrupt everything after it, so the retry
         * on EINTR is not optional. */
        if (n < 0 && errno == EINTR) continue;
        break;
    }
#endif
}

const char* hike_sink_data(const hike_context* ctx, size_t* len_out) {
    if (ctx == NULL || ctx->out_fd >= 0) {
        if (len_out) *len_out = 0;
        return "";
    }
    if (len_out) *len_out = ctx->sink_len;
    return ctx->sink ? ctx->sink : "";
}

void hike_sink_clear(hike_context* ctx) {
    if (ctx == NULL) return;
    ctx->sink_len = 0;
    if (ctx->sink) ctx->sink[0] = '\0';
}

/* A frame is assembled in this buffer and handed to hike_out in large pieces.
 * One write syscall per frame instead of one per cell is worth having on a
 * real terminal, and it costs nothing on a memory sink. */
typedef struct {
    hike_context* ctx;
    char buf[4096];
    size_t len;
} hike_writer;

static void wflush(hike_writer* w) {
    if (w->len == 0) return;
    hike_out(w->ctx, w->buf, w->len);
    w->len = 0;
}

static void wbytes(hike_writer* w, const char* s, size_t n) {
    if (n > sizeof w->buf) { wflush(w); hike_out(w->ctx, s, n); return; }
    if (w->len + n > sizeof w->buf) wflush(w);
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void wstr(hike_writer* w, const char* s) { wbytes(w, s, strlen(s)); }

static void wnum(hike_writer* w, unsigned value) {
    char tmp[12];
    int i = 0;
    if (value == 0) tmp[i++] = '0';
    while (value > 0) { tmp[i++] = (char)('0' + value % 10); value /= 10; }
    char out[12];
    for (int j = 0; j < i; ++j) out[j] = tmp[i - 1 - j];
    wbytes(w, out, (size_t)i);
}

/* ------------------------------------------------------ colour reduction --- */

/* The six levels of the xterm colour cube, and where the boundaries between
 * them fall. Values are the ones xterm actually renders, not an even ramp:
 * rounding to an even ramp visibly shifts dark colours. */
static const int k_cube_levels[6] = {0, 95, 135, 175, 215, 255};

static int nearest_cube_level(int v) {
    int best = 0, best_d = 1 << 30;
    for (int i = 0; i < 6; ++i) {
        int d = v - k_cube_levels[i];
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* The first sixteen xterm palette entries, as most terminals render them.
 * Approximate by nature -- a terminal is free to theme these -- but a nearest
 * match against the conventional values is the best any reduction can do
 * without asking the terminal, and it keeps relative brightness right, which
 * is what legibility actually depends on. */
static const uint8_t k_ansi16[16][3] = {
    {  0,   0,   0}, {170,   0,   0}, {  0, 170,   0}, {170,  85,   0},
    {  0,   0, 170}, {170,   0, 170}, {  0, 170, 170}, {170, 170, 170},
    { 85,  85,  85}, {255,  85,  85}, { 85, 255,  85}, {255, 255,  85},
    { 85,  85, 255}, {255,  85, 255}, { 85, 255, 255}, {255, 255, 255}
};

static void index256_to_rgb(uint8_t index, int* r, int* g, int* b) {
    if (index < 16) {
        *r = k_ansi16[index][0];
        *g = k_ansi16[index][1];
        *b = k_ansi16[index][2];
    } else if (index < 232) {
        int c = index - 16;
        *r = k_cube_levels[(c / 36) % 6];
        *g = k_cube_levels[(c / 6) % 6];
        *b = k_cube_levels[c % 6];
    } else {
        int level = 8 + (index - 232) * 10;
        *r = *g = *b = level;
    }
}

static uint8_t rgb_to_256(int r, int g, int b) {
    /* Greys get the 24-step ramp, which is much finer than the cube's six
     * levels along the diagonal, so a grey UI does not band. */
    if (r == g && g == b) {
        if (r < 8) return 16;              /* the cube's black */
        if (r > 248) return 231;           /* the cube's white */
        int step = (r - 8) / 10;
        if (step > 23) step = 23;
        return (uint8_t)(232 + step);
    }
    int ri = nearest_cube_level(r);
    int gi = nearest_cube_level(g);
    int bi = nearest_cube_level(b);
    return (uint8_t)(16 + 36 * ri + 6 * gi + bi);
}

static uint8_t rgb_to_16(int r, int g, int b) {
    int best = 0;
    long best_d = -1;
    for (int i = 0; i < 16; ++i) {
        long dr = r - k_ansi16[i][0];
        long dg = g - k_ansi16[i][1];
        long db = b - k_ansi16[i][2];
        long d = dr * dr + dg * dg + db * db;
        if (best_d < 0 || d < best_d) { best_d = d; best = i; }
    }
    return (uint8_t)best;
}

uint8_t hike_color_to_256(hike_color c) {
    if (c.kind == HIKE_COLOR_INDEXED) return c.index;
    if (c.kind == HIKE_COLOR_RGB) return rgb_to_256(c.r, c.g, c.b);
    return 0;
}

uint8_t hike_color_to_16(hike_color c) {
    if (c.kind == HIKE_COLOR_INDEXED) {
        if (c.index < 16) return c.index;
        int r, g, b;
        index256_to_rgb(c.index, &r, &g, &b);
        return rgb_to_16(r, g, b);
    }
    if (c.kind == HIKE_COLOR_RGB) return rgb_to_16(c.r, c.g, c.b);
    return 0;
}

static bool color_equal(hike_color a, hike_color b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case HIKE_COLOR_DEFAULT: return true;
        case HIKE_COLOR_INDEXED: return a.index == b.index;
        case HIKE_COLOR_RGB: return a.r == b.r && a.g == b.g && a.b == b.b;
    }
    return false;
}

/* Emits one colour at the depth the terminal has. `base` is 38 for foreground
 * and 48 for background, which is also what the 16-colour offsets derive from,
 * so the two cases stay in step. */
static void emit_color(hike_writer* w, hike_color c, hike_color_depth depth,
                       bool foreground) {
    if (depth == HIKE_COLOR_NONE) return;
    if (c.kind == HIKE_COLOR_DEFAULT) return; /* the leading reset covered it */

    if (depth == HIKE_COLOR_TRUE && c.kind == HIKE_COLOR_RGB) {
        wstr(w, foreground ? ";38;2;" : ";48;2;");
        wnum(w, c.r); wstr(w, ";");
        wnum(w, c.g); wstr(w, ";");
        wnum(w, c.b);
        return;
    }
    if (depth == HIKE_COLOR_TRUE || depth == HIKE_COLOR_256) {
        uint8_t idx = hike_color_to_256(c);
        wstr(w, foreground ? ";38;5;" : ";48;5;");
        wnum(w, idx);
        return;
    }

    /* Sixteen colours are the old SGR numbers: 30-37 and 90-97 for foreground,
     * 40-47 and 100-107 for background. */
    uint8_t idx = hike_color_to_16(c);
    unsigned code;
    if (idx < 8) code = (foreground ? 30u : 40u) + idx;
    else code = (foreground ? 90u : 100u) + (idx - 8u);
    wstr(w, ";");
    wnum(w, code);
}

static void emit_style(hike_writer* w, hike_color fg, hike_color bg,
                       uint16_t attrs, hike_color_depth depth) {
    /* Every style change starts from a reset. Turning individual attributes
     * off has more sequences to get right than it saves bytes, and getting one
     * wrong leaves a stuck attribute smeared across the screen. */
    wstr(w, "\x1b[0");
    if (attrs & HIKE_BOLD) wstr(w, ";1");
    if (attrs & HIKE_DIM) wstr(w, ";2");
    if (attrs & HIKE_ITALIC) wstr(w, ";3");
    if (attrs & HIKE_UNDERLINE) wstr(w, ";4");
    if (attrs & HIKE_REVERSE) wstr(w, ";7");
    if (attrs & HIKE_STRIKETHROUGH) wstr(w, ";9");
    emit_color(w, fg, depth, true);
    emit_color(w, bg, depth, false);
    wstr(w, "m");
}

static void emit_move(hike_writer* w, int x, int y) {
    /* The wire is 1-based; the API is 0-based, because everything else in a
     * grid is. Converting in exactly one place is what keeps that from
     * becoming an off-by-one hunt. */
    wstr(w, "\x1b[");
    wnum(w, (unsigned)(y + 1));
    wstr(w, ";");
    wnum(w, (unsigned)(x + 1));
    wstr(w, "H");
}

/* ------------------------------------------------------------- present --- */

static bool cell_equal(hike_cell a, hike_cell b) {
    return a.ch == b.ch && a.attrs == b.attrs &&
           color_equal(a.fg, b.fg) && color_equal(a.bg, b.bg);
}

void hike_present(hike_context* ctx) {
    if (ctx == NULL || ctx->back == NULL) return;

    hike_writer w;
    w.ctx = ctx;
    w.len = 0;

    bool full = ctx->force_full_repaint;
    bool wrote_anything = false;

    /* Cursor position as the terminal understands it, and whether we know it.
     * Unknown at the start of a frame because anything could have moved it
     * since the last one, and after a write in the last column because the
     * terminal's pending-wrap state makes the answer ambiguous. */
    bool cursor_known = false;
    int cur_x = 0, cur_y = 0;

    bool style_known = false;
    hike_color style_fg = hike_default_color();
    hike_color style_bg = hike_default_color();
    uint16_t style_attrs = 0;

    for (int y = 0; y < ctx->h; ++y) {
        const size_t row = (size_t)y * (size_t)ctx->w;
        int x = 0;
        while (x < ctx->w) {
            hike_cell back = ctx->back[row + (size_t)x];

            /* A continuation reached on its own means its lead was unchanged,
             * so the pair is already correct on screen. It is never emitted:
             * writing the lead advances the terminal over both columns. */
            if (back.ch == HIKE_CELL_CONTINUATION) { ++x; continue; }

            int cw = hike_char_width(back.ch) == 2 ? 2 : 1;
            bool dirty = full || !cell_equal(back, ctx->front[row + (size_t)x]);
            /* A wide character is one glyph in two cells, so a difference in
             * either half has to redraw the lead. */
            if (!dirty && cw == 2 && x + 1 < ctx->w)
                dirty = !cell_equal(ctx->back[row + (size_t)x + 1],
                                    ctx->front[row + (size_t)x + 1]);
            if (!dirty) { x += cw; continue; }

            if (!cursor_known || cur_x != x || cur_y != y) {
                emit_move(&w, x, y);
                cur_x = x; cur_y = y; cursor_known = true;
            }
            wrote_anything = true;

            /* Run of changed cells. Staying inside this loop is the whole
             * point: one cursor move and one style change can cover a whole
             * line of changes. */
            while (x < ctx->w) {
                hike_cell c = ctx->back[row + (size_t)x];
                if (c.ch == HIKE_CELL_CONTINUATION) {
                    /* Only reachable when the lead was emitted just now, and
                     * that emission already moved the terminal past this
                     * column. Copy it forward and carry on. */
                    ctx->front[row + (size_t)x] = c;
                    ++x;
                    continue;
                }
                int cwidth = hike_char_width(c.ch) == 2 ? 2 : 1;
                bool d = full || !cell_equal(c, ctx->front[row + (size_t)x]);
                if (!d && cwidth == 2 && x + 1 < ctx->w)
                    d = !cell_equal(ctx->back[row + (size_t)x + 1],
                                    ctx->front[row + (size_t)x + 1]);
                if (!d) break;

                /* A wide character whose second column would fall off the row
                 * cannot be drawn; a blank keeps the row's width honest
                 * instead of letting the terminal wrap half a glyph. */
                uint32_t cp = c.ch;
                if (cwidth == 2 && x + 1 >= ctx->w) { cp = ' '; cwidth = 1; }
                if (cp == 0 || cp == HIKE_CELL_CONTINUATION) cp = ' ';

                if (!style_known || !color_equal(style_fg, c.fg) ||
                    !color_equal(style_bg, c.bg) || style_attrs != c.attrs) {
                    emit_style(&w, c.fg, c.bg, c.attrs, ctx->depth);
                    style_fg = c.fg; style_bg = c.bg; style_attrs = c.attrs;
                    style_known = true;
                }

                char utf8[4];
                size_t n = hike_utf8_encode(cp, utf8);
                if (n == 0) { utf8[0] = ' '; n = 1; }
                wbytes(&w, utf8, n);

                ctx->front[row + (size_t)x] = c;
                if (cwidth == 2 && x + 1 < ctx->w)
                    ctx->front[row + (size_t)x + 1] =
                        ctx->back[row + (size_t)x + 1];

                x += cwidth;
                cur_x += cwidth;
                /* At or past the last column the terminal is in its pending
                 * wrap state, where the cursor's real position depends on
                 * behaviour we would rather not depend on. Forget it and pay
                 * for one move next time. */
                if (cur_x >= ctx->w) cursor_known = false;
            }
        }
    }

    /* The cursor is placed last so it ends where the caller asked rather than
     * wherever the final run left it. */
    bool cursor_changed = ctx->cursor_visible != ctx->shown_cursor_visible ||
                          (ctx->cursor_visible &&
                           (ctx->cursor_x != ctx->shown_cursor_x ||
                            ctx->cursor_y != ctx->shown_cursor_y));
    if (wrote_anything && ctx->cursor_visible) cursor_changed = true;

    if (cursor_changed) {
        if (ctx->cursor_visible) {
            emit_move(&w, ctx->cursor_x, ctx->cursor_y);
            wstr(&w, "\x1b[?25h");
        } else {
            wstr(&w, "\x1b[?25l");
        }
        ctx->shown_cursor_visible = ctx->cursor_visible;
        ctx->shown_cursor_x = ctx->cursor_x;
        ctx->shown_cursor_y = ctx->cursor_y;
    }

    if (wrote_anything && style_known) {
        /* Leave the terminal in a neutral state. Anything else printed by the
         * program or the shell after us would otherwise inherit our colours. */
        wstr(&w, "\x1b[0m");
    }

    wflush(&w);
    ctx->force_full_repaint = false;
}

void hike_set_cursor(hike_context* ctx, int x, int y, bool visible) {
    if (ctx == NULL) return;
    ctx->cursor_x = x;
    ctx->cursor_y = y;
    ctx->cursor_visible = visible;
}

/* ------------------------------------------------------- context lifetime --- */

static hike_context* context_alloc(void) {
    hike_context* ctx = (hike_context*)calloc(1, sizeof(hike_context));
    if (ctx == NULL) return NULL;
    ctx->out_fd = -1;
    ctx->depth = HIKE_COLOR_NONE;
    ctx->options = hike_default_options();
    ctx->shown_cursor_x = -1;
    ctx->shown_cursor_y = -1;
    ctx->shown_cursor_visible = false;
    ctx->cursor_visible = false;
    return ctx;
}

void hike_context_free(hike_context* ctx) {
    if (ctx == NULL) return;
    hike_buffers_free(ctx);
    free(ctx->sink);
    free(ctx);
}

hike_context* hike_context_new_memory(int w, int h, hike_color_depth depth) {
    if (w <= 0 || h <= 0) return NULL;
    hike_context* ctx = context_alloc();
    if (ctx == NULL) return NULL;
    ctx->depth = depth;
    if (hike_buffers_alloc(ctx, w, h) != HIKE_OK) {
        hike_context_free(ctx);
        return NULL;
    }
    return ctx;
}

/* -------------------------------------------------------- colour depth --- */

static bool env_contains(const char* name, const char* needle) {
    const char* v = getenv(name);
    return v != NULL && strstr(v, needle) != NULL;
}

static hike_color_depth detect_depth(void) {
    /* NO_COLOR is a user saying, for every program on the machine, that they
     * do not want colour. Honouring it costs nothing and ignoring it is rude. */
    const char* no_color = getenv("NO_COLOR");
    if (no_color != NULL && no_color[0] != '\0') return HIKE_COLOR_NONE;

    const char* term = getenv("TERM");
    if (term != NULL && (strcmp(term, "dumb") == 0 || term[0] == '\0'))
        return HIKE_COLOR_NONE;

    if (env_contains("COLORTERM", "truecolor") ||
        env_contains("COLORTERM", "24bit"))
        return HIKE_COLOR_TRUE;

    if (term != NULL && (strstr(term, "256color") != NULL ||
                         strstr(term, "direct") != NULL))
        return HIKE_COLOR_256;

    /* Windows Terminal and the modern conhost both do truecolour, and neither
     * sets TERM, so there is nothing else to go on there. */
#ifdef _WIN32
    if (term == NULL) return HIKE_COLOR_TRUE;
#endif

    if (term == NULL) return HIKE_COLOR_NONE;
    return HIKE_COLOR_16;
}

/* ------------------------------------------------------------ raw mode --- */

/* The setup and teardown sequences, built from the options once so the signal
 * handler can write the teardown without allocating or formatting anything. */
static void write_setup(hike_context* ctx) {
    if (ctx->options.alternate_screen) hike_out(ctx, "\x1b[?1049h", 8);
    if (ctx->options.mouse) hike_out(ctx, "\x1b[?1000h\x1b[?1002h\x1b[?1006h", 24);
    if (ctx->options.bracketed_paste) hike_out(ctx, "\x1b[?2004h", 8);
    if (ctx->options.focus_events) hike_out(ctx, "\x1b[?1004h", 8);
    if (ctx->options.hide_cursor) hike_out(ctx, "\x1b[?25l", 6);
    hike_out(ctx, "\x1b[0m\x1b[2J\x1b[H", 11);
}

#ifndef _WIN32

/* The context the signal handler restores. A single global because there is
 * one terminal, and because a handler cannot be passed an argument. */
static hike_context* volatile g_signal_ctx = NULL;

static const int k_fatal_signals[] = {
    SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGABRT, SIGFPE, SIGBUS, SIGSEGV,
    SIGPIPE, SIGALRM, SIGTERM
};
enum { k_fatal_count = (int)(sizeof k_fatal_signals / sizeof k_fatal_signals[0]) };

static struct sigaction g_previous[k_fatal_count];
static bool g_handlers_installed = false;

/* Async-signal-safe teardown: tcsetattr and write are on the safe list,
 * printf and free are not, so nothing here does anything else.
 *
 * What this cannot cover, stated plainly: SIGKILL and SIGSTOP cannot be
 * caught, so `kill -9` still leaves raw mode on; a process that calls _exit
 * without hike_shutdown does too, since no handler runs; and if the process is
 * already so corrupt that the handler faults, the second fault is fatal
 * immediately. Nothing inside a process can fix the first two, which is why
 * the alternate screen is worth using -- a terminal emulator restoring the
 * primary screen buffer hides most of the damage even when we cannot. */
static void restore_from_signal(int sig) {
    hike_context* ctx = g_signal_ctx;
    if (ctx != NULL && ctx->out_fd >= 0) {
        if (ctx->options.focus_events) (void)!write(ctx->out_fd, "\x1b[?1004l", 8);
        if (ctx->options.bracketed_paste) (void)!write(ctx->out_fd, "\x1b[?2004l", 8);
        if (ctx->options.mouse)
            (void)!write(ctx->out_fd, "\x1b[?1006l\x1b[?1002l\x1b[?1000l", 24);
        (void)!write(ctx->out_fd, "\x1b[0m\x1b[?25h", 10);
        if (ctx->options.alternate_screen)
            (void)!write(ctx->out_fd, "\x1b[?1049l", 8);
        if (ctx->raw_active) (void)tcsetattr(ctx->out_fd, TCSANOW, &ctx->saved_termios);
    }

    /* Re-raise with the previous disposition, so the process dies exactly as
     * it would have and a crash still produces a core file. Restoring the old
     * handler first means a handler that was already installed by the program
     * still gets its turn. */
    for (int i = 0; i < k_fatal_count; ++i) {
        if (k_fatal_signals[i] == sig) {
            (void)sigaction(sig, &g_previous[i], NULL);
            break;
        }
    }
    raise(sig);
}

static void install_handlers(hike_context* ctx) {
    g_signal_ctx = ctx;
    if (g_handlers_installed) return;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = restore_from_signal;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: these handlers do not return. */
    sa.sa_flags = 0;

    for (int i = 0; i < k_fatal_count; ++i) {
        struct sigaction old;
        if (sigaction(k_fatal_signals[i], &old, NULL) != 0) continue;
        /* A signal the program has already chosen to ignore stays ignored.
         * Overriding that would change behaviour the program asked for, and a
         * program ignoring SIGPIPE is a common and deliberate choice. */
        if (old.sa_handler == SIG_IGN) { g_previous[i] = old; continue; }
        g_previous[i] = old;
        (void)sigaction(k_fatal_signals[i], &sa, NULL);
    }
    g_handlers_installed = true;
}

static void remove_handlers(void) {
    if (!g_handlers_installed) return;
    for (int i = 0; i < k_fatal_count; ++i)
        (void)sigaction(k_fatal_signals[i], &g_previous[i], NULL);
    g_handlers_installed = false;
    g_signal_ctx = NULL;
}

static hike_status enter_raw(hike_context* ctx) {
    struct termios raw;
    if (tcgetattr(ctx->out_fd, &ctx->saved_termios) != 0)
        return HIKE_ERR_TERMINAL_SETUP;
    raw = ctx->saved_termios;

    /* The canonical raw mode: no echo, no line discipline, no signal
     * generation from keys (the caller wants Ctrl-C as a key event), no
     * software flow control, and no output post-processing so a newline is
     * not silently turned into a carriage return and newline in the middle of
     * a frame we are positioning by hand. */
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cflag |= (tcflag_t)CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(ctx->out_fd, TCSAFLUSH, &raw) != 0)
        return HIKE_ERR_TERMINAL_SETUP;
    ctx->raw_active = true;
    return HIKE_OK;
}

static void query_size(hike_context* ctx, int* w, int* h) {
    struct winsize ws;
    /* A terminal that cannot report its size is usually one at the far end of
     * something that does not forward the ioctl. 80x24 is the conventional
     * answer and is better than refusing to run. */
    *w = 80;
    *h = 24;
    if (ioctl(ctx->out_fd, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) *w = ws.ws_col;
        if (ws.ws_row > 0) *h = ws.ws_row;
    }
}

#endif /* !_WIN32 */

hike_status hike_init(hike_context** out, const hike_options* options) {
    if (out == NULL) return HIKE_ERR_INVALID_ARGUMENT;
    *out = NULL;

#ifdef _WIN32
    /* UNTESTED. This path is written from the console API documentation and
     * has not been run on Windows; it is here rather than omitted because a
     * missing platform is harder to fix later than a wrong one, but treat any
     * behaviour here as unverified.
     *
     * ENABLE_VIRTUAL_TERMINAL_PROCESSING is what makes the rest of libhike
     * meaningful on Windows: with it the console interprets the same VT
     * sequences everything else here writes. ENABLE_VIRTUAL_TERMINAL_INPUT on
     * the input handle is its counterpart, making keys arrive as the same
     * escape sequences a Unix terminal sends, so the input decoder does not
     * need a second implementation. */
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    if (hout == INVALID_HANDLE_VALUE || GetFileType(hout) != FILE_TYPE_CHAR)
        return HIKE_ERR_NOT_A_TERMINAL;

    DWORD out_mode = 0, in_mode = 0;
    if (!GetConsoleMode(hout, &out_mode)) return HIKE_ERR_NOT_A_TERMINAL;
    (void)GetConsoleMode(hin, &in_mode);

    hike_context* ctx = context_alloc();
    if (ctx == NULL) return HIKE_ERR_OUT_OF_MEMORY;
    if (options != NULL) ctx->options = *options;
    ctx->saved_out_mode = out_mode;
    ctx->saved_in_mode = in_mode;
    ctx->out_fd = _fileno(stdout);

    DWORD want_out = out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                     DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(hout, want_out)) {
        hike_context_free(ctx);
        return HIKE_ERR_TERMINAL_SETUP;
    }
    DWORD want_in = (in_mode & (DWORD)~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                        ENABLE_PROCESSED_INPUT)) |
                    ENABLE_VIRTUAL_TERMINAL_INPUT;
    (void)SetConsoleMode(hin, want_in);
    ctx->raw_active = true;

    int w = 80, h = 24;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(hout, &info)) {
        w = info.srWindow.Right - info.srWindow.Left + 1;
        h = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (w <= 0) w = 80;
        if (h <= 0) h = 24;
    }
    ctx->depth = detect_depth();

    hike_status st = hike_buffers_alloc(ctx, w, h);
    if (st != HIKE_OK) {
        SetConsoleMode(hout, out_mode);
        hike_context_free(ctx);
        return st;
    }
    /* Windows has no signals worth restoring a console from; a console
     * control handler would be the equivalent and is left for the input layer,
     * which already owns the console input handle. */
    write_setup(ctx);
    /* The screen was just cleared, so the front buffer matches it and only
     * real content needs writing. */
    {
        size_t count = (size_t)w * (size_t)h;
        for (size_t i = 0; i < count; ++i) ctx->front[i] = ctx->back[i];
        ctx->force_full_repaint = false;
    }
    *out = ctx;
    return HIKE_OK;
#else
    int fd = fileno(stdout);
    if (fd < 0 || !isatty(fd)) return HIKE_ERR_NOT_A_TERMINAL;

    hike_context* ctx = context_alloc();
    if (ctx == NULL) return HIKE_ERR_OUT_OF_MEMORY;
    if (options != NULL) ctx->options = *options;
    ctx->out_fd = fd;
    ctx->depth = detect_depth();

    hike_status st = enter_raw(ctx);
    if (st != HIKE_OK) {
        hike_context_free(ctx);
        return st;
    }

    int w, h;
    query_size(ctx, &w, &h);
    st = hike_buffers_alloc(ctx, w, h);
    if (st != HIKE_OK) {
        (void)tcsetattr(ctx->out_fd, TCSANOW, &ctx->saved_termios);
        hike_context_free(ctx);
        return st;
    }

    /* Handlers go in only once raw mode is on and there is something to
     * restore, so a failure above leaves no handler pointing at a context that
     * never existed. */
    install_handlers(ctx);
    write_setup(ctx);

    /* The screen has just been cleared, so the front buffer honestly describes
     * it: blank. Copying the blank back buffer over means the first present
     * writes the frame's real content and not a screenful of spaces. */
    {
        size_t count = (size_t)w * (size_t)h;
        for (size_t i = 0; i < count; ++i) ctx->front[i] = ctx->back[i];
        ctx->force_full_repaint = false;
    }
    ctx->shown_cursor_visible = !ctx->options.hide_cursor;

    *out = ctx;
    return HIKE_OK;
#endif
}

void hike_shutdown(hike_context* ctx) {
    if (ctx == NULL) return;

    if (ctx->out_fd >= 0) {
        if (ctx->options.focus_events) hike_out(ctx, "\x1b[?1004l", 8);
        if (ctx->options.bracketed_paste) hike_out(ctx, "\x1b[?2004l", 8);
        if (ctx->options.mouse)
            hike_out(ctx, "\x1b[?1006l\x1b[?1002l\x1b[?1000l", 24);
        hike_out(ctx, "\x1b[0m\x1b[?25h", 10);
        if (ctx->options.alternate_screen) hike_out(ctx, "\x1b[?1049l", 8);
#ifndef _WIN32
        if (ctx->raw_active) (void)tcsetattr(ctx->out_fd, TCSANOW, &ctx->saved_termios);
        remove_handlers();
#else
        if (ctx->raw_active) {
            SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE),
                           (DWORD)ctx->saved_out_mode);
            SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),
                           (DWORD)ctx->saved_in_mode);
        }
#endif
        ctx->raw_active = false;
    }
    hike_context_free(ctx);
}
