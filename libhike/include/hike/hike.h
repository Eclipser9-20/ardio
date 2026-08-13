/* libhike -- a cross-platform terminal user interface library.
 *
 * This is the C core. Everything libhike can do is reachable from here, and
 * the C++ layer in hike.hpp is a wrapper over exactly this surface with no
 * private back door. That constraint is deliberate: it keeps the C API a
 * first-class way to use the library rather than a subset left to rot.
 *
 * There is no ncurses underneath, and no terminfo database. libhike writes
 * ANSI/VT sequences directly and puts the terminal into raw mode itself. The
 * reason is portability in the direction that actually matters here: the same
 * code has to work on macOS, Linux and a Windows console, and the Windows
 * console speaks VT natively now while it has never spoken terminfo.
 *
 * The model is a grid of cells with a back buffer and a front buffer. You draw
 * into the back buffer as often as you like, then present, and libhike writes
 * only the cells that actually changed. A terminal is a slow, serial device
 * over a pipe, so the cost of a frame is very close to the number of bytes
 * written -- diffing is not an optimisation here, it is the difference between
 * a UI that feels instant and one that visibly repaints.
 */
#ifndef HIKE_H
#define HIKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- colour */

/* Colours are stored in a form that can express what any terminal supports,
 * and are reduced at write time to what the terminal in front of us actually
 * has. A UI written against truecolour still has to be legible on a 16-colour
 * terminal, so the reduction happens inside libhike rather than being every
 * caller's problem. */
typedef enum {
    HIKE_COLOR_DEFAULT = 0, /* the terminal's own foreground/background */
    HIKE_COLOR_INDEXED,     /* 0-255, the xterm palette */
    HIKE_COLOR_RGB
} hike_color_kind;

typedef struct {
    hike_color_kind kind;
    uint8_t index;              /* when kind is INDEXED */
    uint8_t r, g, b;            /* when kind is RGB */
} hike_color;

hike_color hike_rgb(uint8_t r, uint8_t g, uint8_t b);
hike_color hike_indexed(uint8_t index);
hike_color hike_default_color(void);

/* What the terminal can actually display. Detected once at init. */
typedef enum {
    HIKE_COLOR_NONE = 0,
    HIKE_COLOR_16,
    HIKE_COLOR_256,
    HIKE_COLOR_TRUE
} hike_color_depth;

/* ----------------------------------------------------------------- cells */

enum {
    HIKE_BOLD          = 1u << 0,
    HIKE_DIM           = 1u << 1,
    HIKE_ITALIC        = 1u << 2,
    HIKE_UNDERLINE     = 1u << 3,
    HIKE_REVERSE       = 1u << 4,
    HIKE_STRIKETHROUGH = 1u << 5
};

/* One character cell.
 *
 * `ch` is a Unicode code point, not a byte, because a terminal grid is
 * addressed in columns and a multi-byte character occupies one cell (or two,
 * for wide East Asian characters and many emoji). Storing bytes here would
 * make every column calculation wrong for any non-ASCII text. */
/* The `ch` of the second cell of a wide character.
 *
 * A wide character occupies two columns, so the cell to its right holds no
 * character of its own. It cannot simply be blank: a blank is a thing a caller
 * could legitimately write, and the pair has to stay recognisable so that
 * overwriting either half repairs the other rather than leaving half a glyph
 * on screen.
 *
 * The value is not a valid Unicode scalar, so it can never collide with real
 * content. hike_get_cell returns it for such a cell, and hike_set_cell refuses
 * it as input -- a continuation only ever exists because a wide character was
 * written next to it. */
#define HIKE_CELL_CONTINUATION 0xFFFFFFFFu

typedef struct {
    uint32_t ch;
    hike_color fg;
    hike_color bg;
    uint16_t attrs;
} hike_cell;

/* ------------------------------------------------------------- geometry */

typedef struct { int x, y; } hike_point;
typedef struct { int x, y, w, h; } hike_rect;

bool hike_rect_contains(hike_rect r, int x, int y);
hike_rect hike_rect_intersect(hike_rect a, hike_rect b);

/* --------------------------------------------------------------- input */

typedef enum {
    HIKE_EVENT_NONE = 0,
    HIKE_EVENT_KEY,
    HIKE_EVENT_MOUSE,
    HIKE_EVENT_RESIZE,
    HIKE_EVENT_PASTE,   /* bracketed paste, delivered whole */
    HIKE_EVENT_FOCUS
} hike_event_kind;

/* Named keys. Printable input arrives as a code point in hike_key_event::ch
 * with key set to HIKE_KEY_CHAR, so this list only needs the keys that have no
 * character of their own. */
typedef enum {
    HIKE_KEY_CHAR = 0,
    HIKE_KEY_ENTER, HIKE_KEY_ESCAPE, HIKE_KEY_BACKSPACE, HIKE_KEY_TAB,
    HIKE_KEY_UP, HIKE_KEY_DOWN, HIKE_KEY_LEFT, HIKE_KEY_RIGHT,
    HIKE_KEY_HOME, HIKE_KEY_END, HIKE_KEY_PAGE_UP, HIKE_KEY_PAGE_DOWN,
    HIKE_KEY_INSERT, HIKE_KEY_DELETE,
    HIKE_KEY_F1, HIKE_KEY_F2, HIKE_KEY_F3, HIKE_KEY_F4, HIKE_KEY_F5,
    HIKE_KEY_F6, HIKE_KEY_F7, HIKE_KEY_F8, HIKE_KEY_F9, HIKE_KEY_F10,
    HIKE_KEY_F11, HIKE_KEY_F12
} hike_key;

enum {
    HIKE_MOD_SHIFT = 1u << 0,
    HIKE_MOD_ALT   = 1u << 1,
    HIKE_MOD_CTRL  = 1u << 2
};

typedef struct {
    hike_key key;
    uint32_t ch;        /* code point when key is HIKE_KEY_CHAR */
    uint8_t mods;
} hike_key_event;

typedef enum {
    HIKE_MOUSE_PRESS, HIKE_MOUSE_RELEASE, HIKE_MOUSE_MOVE,
    HIKE_MOUSE_SCROLL_UP, HIKE_MOUSE_SCROLL_DOWN
} hike_mouse_kind;

typedef struct {
    hike_mouse_kind kind;
    int x, y;           /* zero-based cell coordinates, not the wire's 1-based */

    /* 0 left, 1 middle, 2 right.
     *
     * SGR reporting also has a fourth encoding for motion with no button held.
     * That arrives as HIKE_MOUSE_MOVE with this field 0, rather than as a
     * fourth button value, because "which button" is not a meaningful question
     * about a movement and inventing a button number for it would make every
     * caller filter one out. */
    int button;
    uint8_t mods;
} hike_mouse_event;

typedef struct {
    hike_event_kind kind;
    hike_key_event key;
    hike_mouse_event mouse;
    /* On RESIZE, the new terminal size in w and h. x and y are always zero:
     * a terminal has no origin to report. This should be a width/height pair
     * rather than a rect with two dead fields, and will be once the widget
     * layer has settled -- reusing the rect type here was convenience, and it
     * invites a caller to read coordinates that mean nothing. */
    hike_rect size;
    const char* paste;     /* on PASTE, UTF-8, owned by libhike until the next poll */
    size_t paste_len;
    bool focused;          /* on FOCUS */
} hike_event;

/* --------------------------------------------------------------- context */

typedef struct hike_context hike_context;

typedef enum {
    HIKE_OK = 0,
    HIKE_ERR_NOT_A_TERMINAL,   /* stdout is a pipe or file */
    HIKE_ERR_TERMINAL_SETUP,
    HIKE_ERR_OUT_OF_MEMORY,
    HIKE_ERR_UNSUPPORTED_PLATFORM,
    HIKE_ERR_INVALID_ARGUMENT
} hike_status;

const char* hike_status_text(hike_status status);

typedef struct {
    bool mouse;             /* report mouse events */
    bool bracketed_paste;
    bool focus_events;
    bool alternate_screen;  /* draw on a separate screen and restore on exit */
    bool hide_cursor;
} hike_options;

hike_options hike_default_options(void);

/* Starts a session: puts the terminal in raw mode, allocates the buffers, and
 * detects size and colour depth.
 *
 * Refuses when the output is not a terminal, rather than emitting escape
 * sequences into a pipe. A program that wants to work in both cases should ask
 * first and fall back to plain output of its own.
 *
 * IMPORTANT: the terminal is left in raw mode until hike_shutdown. A process
 * that exits without calling it leaves the user with an unusable shell, so
 * libhike installs handlers for the fatal signals it can and restores the
 * terminal from them. It cannot help with SIGKILL. */
hike_status hike_init(hike_context** out, const hike_options* options);
void hike_shutdown(hike_context* ctx);

int hike_width(const hike_context* ctx);
int hike_height(const hike_context* ctx);
hike_color_depth hike_depth(const hike_context* ctx);

/* ------------------------------------------------------------- drawing */

void hike_clear(hike_context* ctx);
void hike_set_cell(hike_context* ctx, int x, int y, hike_cell cell);
hike_cell hike_get_cell(const hike_context* ctx, int x, int y);

/* Draws UTF-8 text, returning the number of COLUMNS advanced rather than the
 * number of bytes or code points. They differ for wide characters, and the
 * column count is the only one a caller laying out a line can use. */
int hike_text(hike_context* ctx, int x, int y, const char* utf8,
              hike_color fg, hike_color bg, uint16_t attrs);

void hike_fill(hike_context* ctx, hike_rect r, hike_cell cell);

/* Clipping is a stack so a container can constrain its children and restore
 * what it found, without every widget having to know its parent's bounds. */
void hike_push_clip(hike_context* ctx, hike_rect r);
void hike_pop_clip(hike_context* ctx);

/* Writes the changed cells to the terminal. Nothing appears before this. */
void hike_present(hike_context* ctx);

/* Forces the next present to redraw every cell, for when something outside
 * libhike has written to the terminal and the front buffer is a lie. */
void hike_invalidate(hike_context* ctx);

void hike_set_cursor(hike_context* ctx, int x, int y, bool visible);

/* ---------------------------------------------------------------- events */

/* Waits up to timeout_ms for an event. A negative timeout waits indefinitely;
 * zero polls. Returns false when the timeout expired with nothing to report.
 *
 * A resize is delivered as an event rather than handled silently, because only
 * the caller knows whether its layout can survive one. */
bool hike_poll(hike_context* ctx, hike_event* out, int timeout_ms);

/* --------------------------------------------------------------- utility */

/* Columns a UTF-8 string occupies once printed: zero for combining marks, two
 * for wide characters, one otherwise. Layout that uses strlen instead of this
 * will be wrong for any text outside ASCII. */
int hike_text_width(const char* utf8);
int hike_char_width(uint32_t codepoint);

/* Decodes one code point, returning the bytes consumed, or 0 on invalid input.
 * Invalid UTF-8 is reported rather than substituted, so a caller can decide
 * between refusing the input and showing a replacement character. */
size_t hike_utf8_decode(const char* utf8, size_t len, uint32_t* out);
size_t hike_utf8_encode(uint32_t codepoint, char out[4]);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HIKE_H */
