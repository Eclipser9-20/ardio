/* libhike -- layout and widgets.
 *
 * Everything in this file is written against the public core API in hike.h and
 * nothing else. It never touches struct hike_context, which is opaque here on
 * purpose: if the widget layer needed a private door into the core then the C
 * API would not really be the whole library, and every widget a user writes
 * for themselves would be second class.
 *
 * Two habits run through the drawing code and are worth stating once.
 *
 * Text is emitted a code point at a time rather than through hike_text, even
 * though hike_text exists and is the obvious call. The reason is horizontal
 * scrolling and clipping: a text input showing the middle of a long string has
 * to start drawing part-way through the text and stop part-way through it, and
 * a call that takes a whole string and a starting column can express neither
 * end. Doing the walk here also means a wide character that would straddle the
 * right edge is dropped rather than half-drawn, which is the only correct
 * answer -- half of a wide glyph is a different glyph.
 *
 * Nothing here allocates. A widget is handed the caller's own buffer or array
 * and draws it, so there is no ownership question, no failure path on a draw
 * call, and nothing to free on the way out of an error.
 */

#include "hike/widgets.h"

#include <string.h>

/* ---------------------------------------------------------------- style */

hike_style hike_style_default(void) {
    hike_style s;
    s.fg = hike_default_color();
    s.bg = hike_default_color();
    s.attrs = 0;
    return s;
}

hike_style hike_style_make(hike_color fg, hike_color bg, uint16_t attrs) {
    hike_style s;
    s.fg = fg;
    s.bg = bg;
    s.attrs = attrs;
    return s;
}

/* The default theme uses attributes rather than colours wherever it can.
 * Reverse video and bold exist on every terminal libhike can talk to,
 * including a 16-colour one and a monochrome one, so a UI that has not chosen
 * any colours still reads correctly everywhere. A default theme built out of
 * indexed colours would look deliberate on one terminal and arbitrary on the
 * next. */
hike_theme hike_default_theme(void) {
    hike_theme t;
    t.normal = hike_style_default();
    t.focused = hike_style_default();
    t.focused.attrs = HIKE_BOLD;
    t.selected = hike_style_default();
    t.selected.attrs = HIKE_REVERSE;
    t.disabled = hike_style_default();
    t.disabled.attrs = HIKE_DIM;
    t.accent = hike_style_default();
    return t;
}

/* --------------------------------------------------------------- helpers */

static int int_max(int a, int b) { return a > b ? a : b; }
static int int_min(int a, int b) { return a < b ? a : b; }

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void put(hike_context* ctx, int x, int y, uint32_t ch, hike_style s) {
    hike_cell c;
    c.ch = ch;
    c.fg = s.fg;
    c.bg = s.bg;
    c.attrs = s.attrs;
    hike_set_cell(ctx, x, y, c);
}

static void fill_style(hike_context* ctx, hike_rect r, uint32_t ch, hike_style s) {
    hike_cell c;
    c.ch = ch;
    c.fg = s.fg;
    c.bg = s.bg;
    c.attrs = s.attrs;
    hike_fill(ctx, r, c);
}

/* Columns a string occupies, or 0 for a null pointer, so callers can measure
 * an optional label without a null check at every site. */
static int text_width(const char* utf8) {
    return utf8 ? hike_text_width(utf8) : 0;
}

/* Draws `utf8` on one line, starting at column `x` of the strip [x, x + width),
 * with the first `skip` columns of the TEXT suppressed. Returns the number of
 * columns of the strip that were filled.
 *
 * `skip` is what makes horizontal scrolling work: the text is walked from the
 * beginning either way, because the width of the skipped part is exactly what
 * decides where the visible part starts, and no cheaper starting point exists
 * for UTF-8 with wide characters in it. A character straddling the left edge
 * of the window is dropped rather than clipped, for the same reason as the
 * right edge: there is no such thing as half of a glyph. */
static int draw_text(hike_context* ctx, int x, int y, int width,
                     const char* utf8, int skip, hike_style style) {
    int col = 0;      /* column within the text, before skipping */
    int drawn = 0;    /* columns of the strip filled */
    size_t i = 0;
    size_t len;

    if (!utf8 || width <= 0) return 0;
    len = strlen(utf8);
    if (skip < 0) skip = 0;

    while (i < len) {
        uint32_t cp = 0;
        size_t used = hike_utf8_decode(utf8 + i, len - i, &cp);
        int cw;
        int at;
        if (used == 0) {
            /* Invalid UTF-8. The core reports rather than substitutes, and so
             * do we: show U+FFFD for the offending byte so the user can see
             * where the text went wrong instead of losing the rest of it. */
            cp = 0xFFFD;
            used = 1;
        }
        cw = hike_char_width(cp);
        if (cw <= 0) {
            /* A combining mark occupies no column of its own. Skip it rather
             * than emit it into a cell, where it would be applied to nothing. */
            i += used;
            continue;
        }
        at = col - skip;
        col += cw;
        i += used;
        if (at < 0) continue;                 /* still scrolled off the left */
        if (at + cw > width) break;           /* would straddle the right edge */
        put(ctx, x + at, y, cp, style);
        if (cw == 2) put(ctx, x + at + 1, y, ' ', style);
        drawn = at + cw;
    }
    return drawn;
}

static int align_offset(hike_align align, int width, int content) {
    if (content >= width) return 0;
    if (align == HIKE_ALIGN_CENTER) return (width - content) / 2;
    if (align == HIKE_ALIGN_RIGHT) return width - content;
    return 0;
}

/* --------------------------------------------------------------- layout */

hike_size hike_fixed(int cells) {
    hike_size s;
    s.kind = HIKE_SIZE_FIXED;
    s.value = int_max(0, cells);
    return s;
}

hike_size hike_content(int cells) {
    hike_size s;
    s.kind = HIKE_SIZE_CONTENT;
    s.value = int_max(0, cells);
    return s;
}

hike_size hike_weight(int weight) {
    hike_size s;
    s.kind = HIKE_SIZE_WEIGHT;
    s.value = int_max(0, weight);
    return s;
}

hike_layout hike_row(void) {
    hike_layout l;
    memset(&l, 0, sizeof l);
    l.dir = HIKE_LAYOUT_ROW;
    return l;
}

hike_layout hike_column(void) {
    hike_layout l = hike_row();
    l.dir = HIKE_LAYOUT_COLUMN;
    return l;
}

hike_layout hike_layout_pad(hike_layout l, int all) {
    all = int_max(0, all);
    l.pad_left = l.pad_right = l.pad_top = l.pad_bottom = all;
    return l;
}

hike_rect hike_rect_inset(hike_rect r, int amount) {
    hike_rect out;
    if (amount < 0) amount = 0;
    out.x = r.x + amount;
    out.y = r.y + amount;
    out.w = int_max(0, r.w - 2 * amount);
    out.h = int_max(0, r.h - 2 * amount);
    return out;
}

int hike_layout_split(hike_layout layout, hike_rect area,
                      const hike_size* sizes, int n, hike_rect* out) {
    hike_rect inner;
    int extent, cross, gap, gaps, avail, remaining, total_weight, leftover;
    int distributed, weighted_seen, pos, i;

    if (n < 0 || !out) return 0;
    if (n > 0 && !sizes) return 0;
    if (n == 0) return 0;

    inner.x = area.x + int_max(0, layout.pad_left);
    inner.y = area.y + int_max(0, layout.pad_top);
    inner.w = int_max(0, area.w - int_max(0, layout.pad_left) - int_max(0, layout.pad_right));
    inner.h = int_max(0, area.h - int_max(0, layout.pad_top) - int_max(0, layout.pad_bottom));

    if (layout.dir == HIKE_LAYOUT_ROW) {
        extent = inner.w;
        cross = inner.h;
    } else {
        extent = inner.h;
        cross = inner.w;
    }

    /* Gaps are taken off the top, and only as many as there is room for. A gap
     * that does not fit is dropped entirely rather than made narrower, because
     * the alternative is a row where the visible spacing between two children
     * differs from the spacing between the next two. */
    gap = int_max(0, layout.gap);
    gaps = int_min(gap * (n - 1), extent);
    avail = extent - gaps;

    /* Pass one: the sizes that do not depend on anything else, in declaration
     * order, each taking what it asks for or what is left, whichever is less.
     * The out array doubles as scratch for the resolved extents. */
    remaining = avail;
    total_weight = 0;
    for (i = 0; i < n; ++i) {
        int want;
        if (sizes[i].kind == HIKE_SIZE_WEIGHT) {
            out[i].w = 0;
            total_weight += int_max(0, sizes[i].value);
            continue;
        }
        want = int_min(int_max(0, sizes[i].value), remaining);
        out[i].w = want;
        remaining -= want;
    }

    /* Pass two: the leftover, split by weight. floor() for the shares, then
     * the cells lost to flooring handed out one each from the left, so the
     * shares sum to exactly the leftover and the earlier child is the wider
     * one when the division is not exact. */
    leftover = remaining;
    distributed = 0;
    weighted_seen = 0;
    if (total_weight > 0) {
        for (i = 0; i < n; ++i) {
            int w;
            if (sizes[i].kind != HIKE_SIZE_WEIGHT) continue;
            w = int_max(0, sizes[i].value);
            out[i].w = (int)(((long)leftover * (long)w) / (long)total_weight);
            distributed += out[i].w;
            weighted_seen += 1;
        }
        (void)weighted_seen;
        for (i = 0; i < n && distributed < leftover; ++i) {
            if (sizes[i].kind != HIKE_SIZE_WEIGHT) continue;
            if (int_max(0, sizes[i].value) == 0) continue;
            out[i].w += 1;
            distributed += 1;
        }
    }

    /* Place them. The scratch extent in out[i].w becomes the real geometry
     * here, so the cross axis is written last on the row path. */
    pos = (layout.dir == HIKE_LAYOUT_ROW) ? inner.x : inner.y;
    for (i = 0; i < n; ++i) {
        int size = out[i].w;
        if (layout.dir == HIKE_LAYOUT_ROW) {
            out[i].x = pos;
            out[i].y = inner.y;
            out[i].w = size;
            out[i].h = cross;
        } else {
            out[i].x = inner.x;
            out[i].y = pos;
            out[i].w = cross;
            out[i].h = size;
        }
        pos += size;
        if (i + 1 < n && gaps >= gap) {
            pos += gap;
            gaps -= gap;
        }
    }
    return n;
}

/* --------------------------------------------------------------- focus */

hike_focus hike_focus_make(int count) {
    hike_focus f;
    f.count = int_max(0, count);
    f.index = 0;
    return f;
}

bool hike_focus_has(const hike_focus* f, int index) {
    if (!f || f->count <= 0) return false;
    return clamp_int(f->index, 0, f->count - 1) == index;
}

void hike_focus_set(hike_focus* f, int index) {
    if (!f || f->count <= 0) return;
    f->index = clamp_int(index, 0, f->count - 1);
}

void hike_focus_next(hike_focus* f) {
    if (!f || f->count <= 0) return;
    f->index = clamp_int(f->index, 0, f->count - 1);
    f->index = (f->index + 1) % f->count;
}

void hike_focus_prev(hike_focus* f) {
    if (!f || f->count <= 0) return;
    f->index = clamp_int(f->index, 0, f->count - 1);
    f->index = (f->index + f->count - 1) % f->count;
}

bool hike_focus_key(hike_focus* f, const hike_event* ev) {
    if (!f || !ev || ev->kind != HIKE_EVENT_KEY) return false;
    if (ev->key.key != HIKE_KEY_TAB) return false;
    if (ev->key.mods & HIKE_MOD_SHIFT) hike_focus_prev(f);
    else hike_focus_next(f);
    return true;
}

/* --------------------------------------------------------------- label */

int hike_label(hike_context* ctx, hike_rect r, const char* utf8,
               hike_align align, hike_style style) {
    int offset;
    if (!ctx || r.w <= 0 || r.h <= 0) return 0;
    offset = align_offset(align, r.w, text_width(utf8));
    return draw_text(ctx, r.x + offset, r.y, r.w - offset, utf8, 0, style);
}

/* ----------------------------------------------------------------- box */

/* Corner and edge glyphs per border style, in the order
 * top-left, top-right, bottom-left, bottom-right, horizontal, vertical. */
static const uint32_t* border_glyphs(hike_border_style style) {
    static const uint32_t single[6]  = {0x250C, 0x2510, 0x2514, 0x2518, 0x2500, 0x2502};
    static const uint32_t doubled[6] = {0x2554, 0x2557, 0x255A, 0x255D, 0x2550, 0x2551};
    static const uint32_t rounded[6] = {0x256D, 0x256E, 0x2570, 0x256F, 0x2500, 0x2502};
    static const uint32_t thick[6]   = {0x250F, 0x2513, 0x2517, 0x251B, 0x2501, 0x2503};
    static const uint32_t ascii[6]   = {'+', '+', '+', '+', '-', '|'};
    switch (style) {
        case HIKE_BORDER_DOUBLE:  return doubled;
        case HIKE_BORDER_ROUNDED: return rounded;
        case HIKE_BORDER_THICK:   return thick;
        case HIKE_BORDER_ASCII:   return ascii;
        default:                  return single;
    }
}

hike_rect hike_box(hike_context* ctx, hike_rect r, hike_border_style border,
                   const char* title, hike_style style) {
    const uint32_t* g;
    hike_rect inner;
    int x, y;

    inner.x = r.x;
    inner.y = r.y;
    inner.w = 0;
    inner.h = 0;
    if (!ctx || r.w <= 0 || r.h <= 0) return inner;

    if (border == HIKE_BORDER_NONE) {
        inner = r;
    } else {
        inner.x = r.x + 1;
        inner.y = r.y + 1;
        inner.w = int_max(0, r.w - 2);
        inner.h = int_max(0, r.h - 2);
    }
    if (border == HIKE_BORDER_NONE) return inner;

    g = border_glyphs(border);
    for (x = r.x + 1; x < r.x + r.w - 1; ++x) {
        put(ctx, x, r.y, g[4], style);
        if (r.h > 1) put(ctx, x, r.y + r.h - 1, g[4], style);
    }
    for (y = r.y + 1; y < r.y + r.h - 1; ++y) {
        put(ctx, r.x, y, g[5], style);
        if (r.w > 1) put(ctx, r.x + r.w - 1, y, g[5], style);
    }
    put(ctx, r.x, r.y, g[0], style);
    if (r.w > 1) put(ctx, r.x + r.w - 1, r.y, g[1], style);
    if (r.h > 1) put(ctx, r.x, r.y + r.h - 1, g[2], style);
    if (r.w > 1 && r.h > 1) put(ctx, r.x + r.w - 1, r.y + r.h - 1, g[3], style);

    /* The title sits in the top edge with a space either side, so the border
     * does not run into the letters. It is drawn only when the whole of it
     * fits, padding included: a title cut off mid-word inside a frame reads as
     * a rendering bug rather than as a label that is too long. */
    if (title && title[0] && r.w >= 2) {
        int tw = text_width(title);
        if (tw + 4 <= r.w) {
            put(ctx, r.x + 1, r.y, ' ', style);
            draw_text(ctx, r.x + 2, r.y, tw, title, 0, style);
            put(ctx, r.x + 2 + tw, r.y, ' ', style);
        }
    }
    return inner;
}

/* --------------------------------------------------------------- button */

hike_button hike_button_make(const char* label) {
    hike_button b;
    memset(&b, 0, sizeof b);
    b.label = label;
    b.align = HIKE_ALIGN_CENTER;
    b.theme = hike_default_theme();
    return b;
}

static hike_style widget_style(const hike_theme* t, bool focused, bool disabled) {
    if (disabled) return t->disabled;
    return focused ? t->focused : t->normal;
}

void hike_button_draw(hike_context* ctx, hike_rect r, const hike_button* b) {
    hike_style s;
    int label_w, offset;
    if (!ctx || !b || r.w <= 0 || r.h <= 0) return;

    s = widget_style(&b->theme, b->focused, b->disabled);
    /* A button is filled before the label is drawn so its background reads as
     * one shape even when the label is shorter than the button. */
    fill_style(ctx, (hike_rect){r.x, r.y, r.w, 1}, ' ', s);

    /* Brackets mark it as pressable on a terminal with no colour at all, which
     * is the only cue left when reverse video is all the styling there is. */
    label_w = text_width(b->label) + 4;
    offset = align_offset(b->align, r.w, label_w);
    if (label_w <= r.w) {
        put(ctx, r.x + offset, r.y, '[', s);
        draw_text(ctx, r.x + offset + 2, r.y, r.w - offset - 4, b->label, 0, s);
        put(ctx, r.x + offset + label_w - 1, r.y, ']', s);
    } else {
        draw_text(ctx, r.x, r.y, r.w, b->label, 0, s);
    }
}

bool hike_button_event(const hike_button* b, hike_rect r, const hike_event* ev) {
    if (!b || !ev || b->disabled) return false;
    if (ev->kind == HIKE_EVENT_KEY) {
        if (!b->focused) return false;
        if (ev->key.key == HIKE_KEY_ENTER) return true;
        return ev->key.key == HIKE_KEY_CHAR && ev->key.ch == ' ';
    }
    if (ev->kind == HIKE_EVENT_MOUSE && ev->mouse.kind == HIKE_MOUSE_PRESS) {
        return hike_rect_contains(r, ev->mouse.x, ev->mouse.y);
    }
    return false;
}

/* ---------------------------------------------------------- text input */

hike_input hike_input_make(char* buf, size_t cap) {
    hike_input in;
    memset(&in, 0, sizeof in);
    in.buf = buf;
    in.cap = cap;
    in.theme = hike_default_theme();
    if (buf && cap > 0) {
        buf[0] = '\0';
    }
    return in;
}

void hike_input_set_text(hike_input* in, const char* utf8) {
    size_t n;
    if (!in || !in->buf || in->cap == 0) return;
    n = utf8 ? strlen(utf8) : 0;
    if (n > in->cap - 1) n = in->cap - 1;
    if (n && utf8) memcpy(in->buf, utf8, n);
    in->buf[n] = '\0';
    in->len = n;
    in->cursor = n;
    in->scroll = 0;
}

/* Columns occupied by the first `bytes` bytes of the input's text. Used for
 * both the cursor column and the scroll arithmetic, so they can never disagree
 * about where a character sits. */
static int columns_before(const hike_input* in, size_t bytes) {
    size_t i = 0;
    int col = 0;
    if (!in->buf) return 0;
    if (bytes > in->len) bytes = in->len;
    while (i < bytes) {
        uint32_t cp = 0;
        size_t used = hike_utf8_decode(in->buf + i, in->len - i, &cp);
        int w;
        if (used == 0) { used = 1; cp = 0xFFFD; }
        w = in->masked ? 1 : hike_char_width(cp);
        if (w > 0) col += w;
        i += used;
    }
    return col;
}

int hike_input_cursor_column(const hike_input* in) {
    if (!in) return 0;
    return columns_before(in, in->cursor) - in->scroll;
}

/* Moves `scroll` the least it can so the cursor is inside [0, width). The
 * cursor is allowed to sit one column past the last character -- that is where
 * the next typed character goes -- so a full field scrolls by one as soon as
 * the text reaches the right edge, which is what makes typing at the end feel
 * continuous rather than jumping a field-width at a time. */
static void input_scroll_to_cursor(hike_input* in, int width) {
    int cursor_col;
    if (width <= 0) { in->scroll = 0; return; }
    cursor_col = columns_before(in, in->cursor);
    if (in->scroll > cursor_col) in->scroll = cursor_col;
    if (cursor_col - in->scroll > width - 1) in->scroll = cursor_col - (width - 1);
    if (in->scroll < 0) in->scroll = 0;
}

void hike_input_draw(hike_context* ctx, hike_rect r, hike_input* in) {
    hike_style s;
    if (!ctx || !in || r.w <= 0 || r.h <= 0) return;

    s = in->focused ? in->theme.focused : in->theme.normal;
    fill_style(ctx, (hike_rect){r.x, r.y, r.w, 1}, ' ', s);
    input_scroll_to_cursor(in, r.w);

    if (in->len == 0 && in->placeholder && !in->focused) {
        draw_text(ctx, r.x, r.y, r.w, in->placeholder, 0, in->theme.disabled);
    } else if (in->masked) {
        int i;
        int total = columns_before(in, in->len);
        for (i = in->scroll; i < total && i - in->scroll < r.w; ++i) {
            put(ctx, r.x + (i - in->scroll), r.y, '*', s);
        }
    } else {
        draw_text(ctx, r.x, r.y, r.w, in->buf, in->scroll, s);
    }

    if (in->focused) {
        int col = clamp_int(hike_input_cursor_column(in), 0, r.w - 1);
        hike_set_cursor(ctx, r.x + col, r.y, true);
    }
}

/* Byte offset of the character before `pos`, found by walking forward from the
 * start. UTF-8 can be scanned backwards by looking for a non-continuation
 * byte, but only forward scanning gives the same answer as the decoder does
 * for malformed input, and a cursor that disagrees with the renderer about
 * where characters begin is how text editors corrupt files. */
static size_t prev_char(const hike_input* in, size_t pos) {
    size_t i = 0, last = 0;
    while (i < pos && i < in->len) {
        uint32_t cp = 0;
        size_t used = hike_utf8_decode(in->buf + i, in->len - i, &cp);
        if (used == 0) used = 1;
        last = i;
        i += used;
    }
    return last;
}

static size_t next_char(const hike_input* in, size_t pos) {
    uint32_t cp = 0;
    size_t used;
    if (pos >= in->len) return in->len;
    used = hike_utf8_decode(in->buf + pos, in->len - pos, &cp);
    if (used == 0) used = 1;
    return pos + used > in->len ? in->len : pos + used;
}

static void input_erase(hike_input* in, size_t from, size_t to) {
    if (to <= from || to > in->len) return;
    memmove(in->buf + from, in->buf + to, in->len - to);
    in->len -= (to - from);
    in->buf[in->len] = '\0';
    if (in->cursor > in->len) in->cursor = in->len;
}

static bool input_insert(hike_input* in, const char* bytes, size_t n) {
    if (n == 0) return false;
    if (in->len + n + 1 > in->cap) return false;   /* refuse rather than truncate */
    memmove(in->buf + in->cursor + n, in->buf + in->cursor, in->len - in->cursor);
    memcpy(in->buf + in->cursor, bytes, n);
    in->len += n;
    in->cursor += n;
    in->buf[in->len] = '\0';
    return true;
}

bool hike_input_event(hike_input* in, int width, const hike_event* ev) {
    bool handled = false;
    if (!in || !in->buf || !ev || !in->focused) return false;

    if (ev->kind == HIKE_EVENT_PASTE && ev->paste) {
        handled = input_insert(in, ev->paste, ev->paste_len);
    } else if (ev->kind == HIKE_EVENT_KEY) {
        switch (ev->key.key) {
            case HIKE_KEY_LEFT:
                if (in->cursor > 0) in->cursor = prev_char(in, in->cursor);
                handled = true;
                break;
            case HIKE_KEY_RIGHT:
                in->cursor = next_char(in, in->cursor);
                handled = true;
                break;
            case HIKE_KEY_HOME:
                in->cursor = 0;
                handled = true;
                break;
            case HIKE_KEY_END:
                in->cursor = in->len;
                handled = true;
                break;
            case HIKE_KEY_BACKSPACE:
                if (in->cursor > 0) {
                    size_t from = prev_char(in, in->cursor);
                    size_t to = in->cursor;
                    in->cursor = from;
                    input_erase(in, from, to);
                }
                handled = true;
                break;
            case HIKE_KEY_DELETE:
                input_erase(in, in->cursor, next_char(in, in->cursor));
                handled = true;
                break;
            case HIKE_KEY_CHAR: {
                char enc[4];
                size_t n;
                /* Control characters are not text and are dropped here rather
                 * than inserted as invisible cells the user cannot find. */
                if (ev->key.ch < 0x20 || ev->key.ch == 0x7F) break;
                if (ev->key.mods & HIKE_MOD_CTRL) break;
                n = hike_utf8_encode(ev->key.ch, enc);
                handled = input_insert(in, enc, n);
                break;
            }
            default:
                break;
        }
    }
    if (handled) input_scroll_to_cursor(in, width);
    return handled;
}

/* ------------------------------------------------------------- checkbox */

hike_checkbox hike_checkbox_make(const char* label, bool checked) {
    hike_checkbox c;
    memset(&c, 0, sizeof c);
    c.label = label;
    c.checked = checked;
    c.theme = hike_default_theme();
    return c;
}

void hike_checkbox_draw(hike_context* ctx, hike_rect r, const hike_checkbox* c) {
    hike_style s;
    if (!ctx || !c || r.w <= 0 || r.h <= 0) return;
    s = widget_style(&c->theme, c->focused, c->disabled);
    fill_style(ctx, (hike_rect){r.x, r.y, r.w, 1}, ' ', s);
    if (r.w >= 1) put(ctx, r.x, r.y, '[', s);
    if (r.w >= 2) put(ctx, r.x + 1, r.y, c->checked ? 'x' : ' ', s);
    if (r.w >= 3) put(ctx, r.x + 2, r.y, ']', s);
    if (r.w >= 5) draw_text(ctx, r.x + 4, r.y, r.w - 4, c->label, 0, s);
}

bool hike_checkbox_event(hike_checkbox* c, hike_rect r, const hike_event* ev) {
    if (!c || !ev || c->disabled) return false;
    if (ev->kind == HIKE_EVENT_KEY && c->focused) {
        bool activate = ev->key.key == HIKE_KEY_ENTER ||
                        (ev->key.key == HIKE_KEY_CHAR && ev->key.ch == ' ');
        if (!activate) return false;
        c->checked = !c->checked;
        return true;
    }
    if (ev->kind == HIKE_EVENT_MOUSE && ev->mouse.kind == HIKE_MOUSE_PRESS &&
        hike_rect_contains(r, ev->mouse.x, ev->mouse.y)) {
        c->checked = !c->checked;
        return true;
    }
    return false;
}

/* ---------------------------------------------------------- radio group */

hike_radio_group hike_radio_make(const char* const* labels, int count, int selected) {
    hike_radio_group g;
    memset(&g, 0, sizeof g);
    g.labels = labels;
    g.count = int_max(0, count);
    g.selected = g.count ? clamp_int(selected, 0, g.count - 1) : 0;
    g.theme = hike_default_theme();
    return g;
}

void hike_radio_draw(hike_context* ctx, hike_rect r, const hike_radio_group* g) {
    int i;
    if (!ctx || !g || !g->labels || r.w <= 0) return;
    for (i = 0; i < g->count && i < r.h; ++i) {
        hike_style s = widget_style(&g->theme, g->focused, g->disabled);
        if (i == g->selected && !g->disabled) s = g->theme.selected;
        fill_style(ctx, (hike_rect){r.x, r.y + i, r.w, 1}, ' ', s);
        if (r.w >= 1) put(ctx, r.x, r.y + i, '(', s);
        /* A filled bullet rather than an 'x', because a radio button that
         * looks like a checkbox teaches the user the wrong thing about whether
         * the choices are exclusive. */
        if (r.w >= 2) put(ctx, r.x + 1, r.y + i, i == g->selected ? 0x2022 : ' ', s);
        if (r.w >= 3) put(ctx, r.x + 2, r.y + i, ')', s);
        if (r.w >= 5) draw_text(ctx, r.x + 4, r.y + i, r.w - 4, g->labels[i], 0, s);
    }
}

bool hike_radio_event(hike_radio_group* g, hike_rect r, const hike_event* ev) {
    if (!g || !ev || g->disabled || g->count <= 0) return false;
    if (ev->kind == HIKE_EVENT_KEY && g->focused) {
        int before = g->selected;
        if (ev->key.key == HIKE_KEY_UP || ev->key.key == HIKE_KEY_LEFT) {
            g->selected = clamp_int(g->selected - 1, 0, g->count - 1);
        } else if (ev->key.key == HIKE_KEY_DOWN || ev->key.key == HIKE_KEY_RIGHT) {
            g->selected = clamp_int(g->selected + 1, 0, g->count - 1);
        } else {
            return false;
        }
        return g->selected != before;
    }
    if (ev->kind == HIKE_EVENT_MOUSE && ev->mouse.kind == HIKE_MOUSE_PRESS &&
        hike_rect_contains(r, ev->mouse.x, ev->mouse.y)) {
        int row = ev->mouse.y - r.y;
        if (row >= 0 && row < g->count && row != g->selected) {
            g->selected = row;
            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------------- list */

hike_list hike_list_make(const char* const* items, int count) {
    hike_list l;
    memset(&l, 0, sizeof l);
    l.items = items;
    l.count = int_max(0, count);
    l.theme = hike_default_theme();
    return l;
}

/* Keeps `scroll` in range and the selection on screen, moving by the least it
 * can. Called from both draw and event handling so that a selection changed by
 * the program scrolls into view exactly as one changed by an arrow key does. */
static void list_reveal(hike_list* l, int height) {
    int max_scroll;
    if (height <= 0 || l->count <= 0) { l->scroll = 0; return; }
    l->selected = clamp_int(l->selected, 0, l->count - 1);
    if (l->selected < l->scroll) l->scroll = l->selected;
    if (l->selected > l->scroll + height - 1) l->scroll = l->selected - height + 1;
    max_scroll = int_max(0, l->count - height);
    l->scroll = clamp_int(l->scroll, 0, max_scroll);
}

void hike_list_draw(hike_context* ctx, hike_rect r, hike_list* l) {
    int row;
    if (!ctx || !l || r.w <= 0 || r.h <= 0) return;
    list_reveal(l, r.h);
    for (row = 0; row < r.h; ++row) {
        int index = l->scroll + row;
        hike_style s = l->focused ? l->theme.focused : l->theme.normal;
        if (index < l->count && index == l->selected) s = l->theme.selected;
        fill_style(ctx, (hike_rect){r.x, r.y + row, r.w, 1}, ' ', s);
        if (index >= l->count || !l->items) continue;
        draw_text(ctx, r.x, r.y + row, r.w, l->items[index], 0, s);
    }
}

bool hike_list_event(hike_list* l, int height, const hike_event* ev) {
    int before;
    if (!l || !ev || l->count <= 0) return false;
    if (ev->kind == HIKE_EVENT_MOUSE && !l->focused) return false;
    if (ev->kind == HIKE_EVENT_KEY && !l->focused) return false;
    before = l->selected;

    if (ev->kind == HIKE_EVENT_KEY) {
        int page = int_max(1, height);
        switch (ev->key.key) {
            /* The selection stops at the ends rather than wrapping. Wrapping a
             * list means holding Down past the last item silently returns to
             * the first, and a user who was not watching cannot tell that it
             * happened. */
            case HIKE_KEY_UP:        l->selected -= 1; break;
            case HIKE_KEY_DOWN:      l->selected += 1; break;
            case HIKE_KEY_PAGE_UP:   l->selected -= page; break;
            case HIKE_KEY_PAGE_DOWN: l->selected += page; break;
            case HIKE_KEY_HOME:      l->selected = 0; break;
            case HIKE_KEY_END:       l->selected = l->count - 1; break;
            default: return false;
        }
    } else if (ev->kind == HIKE_EVENT_MOUSE) {
        if (ev->mouse.kind == HIKE_MOUSE_SCROLL_DOWN) l->selected += 1;
        else if (ev->mouse.kind == HIKE_MOUSE_SCROLL_UP) l->selected -= 1;
        else return false;
    } else {
        return false;
    }

    l->selected = clamp_int(l->selected, 0, l->count - 1);
    list_reveal(l, height);
    return l->selected != before;
}

/* ------------------------------------------------------------- progress */

hike_progress hike_progress_make(double value) {
    hike_progress p;
    memset(&p, 0, sizeof p);
    p.value = value;
    p.theme = hike_default_theme();
    return p;
}

void hike_progress_draw(hike_context* ctx, hike_rect r, const hike_progress* p) {
    double v;
    int filled, i;
    if (!ctx || !p || r.w <= 0 || r.h <= 0) return;
    v = p->value;
    if (!(v >= 0.0)) v = 0.0;      /* also catches NaN, which must not draw a bar */
    if (v > 1.0) v = 1.0;

    /* The filled count rounds to nearest, except that it never reaches the
     * full width below 1.0 and never falls short of it at 1.0. A bar that
     * reads full at 99.6% is a bar that lies about being finished. */
    filled = (int)(v * r.w + 0.5);
    if (filled >= r.w && v < 1.0) filled = r.w - 1;
    if (v >= 1.0) filled = r.w;

    for (i = 0; i < r.w; ++i) {
        put(ctx, r.x + i, r.y, i < filled ? 0x2588 : 0x2591,
            i < filled ? p->theme.accent : p->theme.normal);
    }
    if (p->show_percent) {
        char text[8];
        int pct = (int)(v * 100.0 + 0.5);
        int n = 0, offset;
        if (pct >= 100) { text[n++] = '1'; text[n++] = '0'; text[n++] = '0'; }
        else if (pct >= 10) { text[n++] = (char)('0' + pct / 10); text[n++] = (char)('0' + pct % 10); }
        else { text[n++] = (char)('0' + pct); }
        text[n++] = '%';
        text[n] = '\0';
        offset = align_offset(HIKE_ALIGN_CENTER, r.w, n);
        /* Drawn over the bar, so the percentage keeps the style of the half it
         * sits on and stays legible on both. */
        for (i = 0; i < n && offset + i < r.w; ++i) {
            put(ctx, r.x + offset + i, r.y, (uint32_t)(unsigned char)text[i],
                (offset + i) < filled ? p->theme.accent : p->theme.normal);
        }
    }
}

/* ------------------------------------------------------------- textview */

hike_textview hike_textview_make(const char* const* lines, int count) {
    hike_textview v;
    memset(&v, 0, sizeof v);
    v.lines = lines;
    v.count = int_max(0, count);
    v.theme = hike_default_theme();
    return v;
}

void hike_textview_draw(hike_context* ctx, hike_rect r, hike_textview* v) {
    int row;
    hike_style s;
    if (!ctx || !v || r.w <= 0 || r.h <= 0) return;
    s = v->focused ? v->theme.focused : v->theme.normal;
    v->scroll = clamp_int(v->scroll, 0, int_max(0, v->count - 1));
    if (v->hscroll < 0) v->hscroll = 0;
    for (row = 0; row < r.h; ++row) {
        int index = v->scroll + row;
        fill_style(ctx, (hike_rect){r.x, r.y + row, r.w, 1}, ' ', s);
        if (index >= v->count || !v->lines) continue;
        draw_text(ctx, r.x, r.y + row, r.w, v->lines[index], v->hscroll, s);
    }
}

bool hike_textview_event(hike_textview* v, int height, const hike_event* ev) {
    int before_v, before_h, page;
    if (!v || !ev) return false;
    if (ev->kind == HIKE_EVENT_KEY && !v->focused) return false;
    before_v = v->scroll;
    before_h = v->hscroll;
    page = int_max(1, height);

    if (ev->kind == HIKE_EVENT_KEY) {
        switch (ev->key.key) {
            case HIKE_KEY_UP:        v->scroll -= 1; break;
            case HIKE_KEY_DOWN:      v->scroll += 1; break;
            case HIKE_KEY_PAGE_UP:   v->scroll -= page; break;
            case HIKE_KEY_PAGE_DOWN: v->scroll += page; break;
            case HIKE_KEY_HOME:      v->scroll = 0; v->hscroll = 0; break;
            case HIKE_KEY_END:       v->scroll = int_max(0, v->count - page); break;
            case HIKE_KEY_LEFT:      v->hscroll -= 1; break;
            case HIKE_KEY_RIGHT:     v->hscroll += 1; break;
            default: return false;
        }
    } else if (ev->kind == HIKE_EVENT_MOUSE && v->focused) {
        if (ev->mouse.kind == HIKE_MOUSE_SCROLL_DOWN) v->scroll += 1;
        else if (ev->mouse.kind == HIKE_MOUSE_SCROLL_UP) v->scroll -= 1;
        else return false;
    } else {
        return false;
    }

    /* The last line can be scrolled to the top of the view but no further, so
     * there is always something on screen to scroll back from. */
    v->scroll = clamp_int(v->scroll, 0, int_max(0, v->count - 1));
    if (v->hscroll < 0) v->hscroll = 0;
    return v->scroll != before_v || v->hscroll != before_h;
}

/* ----------------------------------------------------------------- tabs */

hike_tabs hike_tabs_make(const char* const* labels, int count) {
    hike_tabs t;
    memset(&t, 0, sizeof t);
    t.labels = labels;
    t.count = int_max(0, count);
    t.theme = hike_default_theme();
    return t;
}

/* Width one tab occupies, label plus a space either side. Kept in one function
 * because drawing and hit-testing must agree exactly; two copies of this
 * arithmetic would drift and clicks would land on the neighbouring tab. */
static int tab_width(const hike_tabs* t, int i) {
    return text_width(t->labels[i]) + 2;
}

void hike_tabs_draw(hike_context* ctx, hike_rect r, const hike_tabs* t) {
    int i, x;
    if (!ctx || !t || !t->labels || r.w <= 0 || r.h <= 0) return;
    fill_style(ctx, (hike_rect){r.x, r.y, r.w, 1}, ' ', t->theme.normal);
    x = r.x;
    for (i = 0; i < t->count; ++i) {
        int w = tab_width(t, i);
        hike_style s = (i == t->selected)
            ? t->theme.selected
            : (t->focused ? t->theme.focused : t->theme.normal);
        if (x + w > r.x + r.w) break;    /* a tab is drawn whole or not at all */
        fill_style(ctx, (hike_rect){x, r.y, w, 1}, ' ', s);
        draw_text(ctx, x + 1, r.y, w - 2, t->labels[i], 0, s);
        x += w;
    }
}

bool hike_tabs_event(hike_tabs* t, hike_rect r, const hike_event* ev) {
    int before;
    if (!t || !ev || t->count <= 0) return false;
    before = t->selected;
    if (ev->kind == HIKE_EVENT_KEY && t->focused) {
        if (ev->key.key == HIKE_KEY_LEFT) t->selected -= 1;
        else if (ev->key.key == HIKE_KEY_RIGHT) t->selected += 1;
        else return false;
        t->selected = clamp_int(t->selected, 0, t->count - 1);
        return t->selected != before;
    }
    if (ev->kind == HIKE_EVENT_MOUSE && ev->mouse.kind == HIKE_MOUSE_PRESS &&
        hike_rect_contains(r, ev->mouse.x, ev->mouse.y) && t->labels) {
        int i, x = r.x;
        for (i = 0; i < t->count; ++i) {
            int w = tab_width(t, i);
            if (ev->mouse.x >= x && ev->mouse.x < x + w) {
                t->selected = i;
                return t->selected != before;
            }
            x += w;
        }
    }
    return false;
}
