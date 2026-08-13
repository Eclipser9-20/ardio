/* The cell grid: allocation, drawing, clipping, and the geometry the rest of
 * the library measures with.
 *
 * There are two grids of identical shape. The back buffer is what a caller
 * draws into and may rewrite as many times as it likes in a frame; the front
 * buffer is what the terminal is believed to be showing. Nothing here talks to
 * a terminal -- hike_present in hike_term.c reconciles the two -- so every
 * function in this file is pure memory work and can be tested without one.
 *
 * Two invariants hold everywhere below, and every write goes through one
 * helper so they cannot be broken by accident:
 *
 *   * Nothing is written outside the active clip rectangle. A widget draws in
 *     its own coordinates and does not have to know where its parent's bounds
 *     are; clipping is what makes that safe.
 *
 *   * A wide character occupies a lead cell and a continuation cell, and the
 *     grid never contains one without the other. Writing over either half
 *     blanks the other, because a half-glyph on screen is not merely ugly --
 *     the terminal's idea of which column it is in stops matching ours for the
 *     rest of that row.
 */

#include "hike_internal.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------- colour --- */

hike_color hike_rgb(uint8_t r, uint8_t g, uint8_t b) {
    hike_color c;
    c.kind = HIKE_COLOR_RGB;
    c.index = 0;
    c.r = r; c.g = g; c.b = b;
    return c;
}

hike_color hike_indexed(uint8_t index) {
    hike_color c;
    c.kind = HIKE_COLOR_INDEXED;
    c.index = index;
    c.r = c.g = c.b = 0;
    return c;
}

hike_color hike_default_color(void) {
    hike_color c;
    c.kind = HIKE_COLOR_DEFAULT;
    c.index = 0;
    c.r = c.g = c.b = 0;
    return c;
}

/* ------------------------------------------------------------ geometry --- */

bool hike_rect_contains(hike_rect r, int x, int y) {
    /* An empty rectangle contains nothing, including its own origin. Saying so
     * here rather than relying on the comparisons is what makes an intersection
     * that came out empty safe to test points against. */
    if (r.w <= 0 || r.h <= 0) return false;
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

hike_rect hike_rect_intersect(hike_rect a, hike_rect b) {
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);

    hike_rect r;
    r.x = x0;
    r.y = y0;
    /* Disjoint rectangles produce a negative extent from that arithmetic.
     * Clamping to zero here means the result is always a rectangle a caller
     * can pass on without checking for a nonsense one. */
    r.w = x1 > x0 ? x1 - x0 : 0;
    r.h = y1 > y0 ? y1 - y0 : 0;
    if (r.w == 0 || r.h == 0) { r.w = 0; r.h = 0; }
    return r;
}

/* ------------------------------------------------------------- buffers --- */

static hike_cell blank_cell(void) {
    hike_cell c;
    c.ch = ' ';
    c.fg = hike_default_color();
    c.bg = hike_default_color();
    c.attrs = 0;
    return c;
}

hike_status hike_buffers_alloc(hike_context* ctx, int w, int h) {
    if (w <= 0 || h <= 0) return HIKE_ERR_INVALID_ARGUMENT;

    size_t count = (size_t)w * (size_t)h;
    hike_cell* back = (hike_cell*)calloc(count, sizeof(hike_cell));
    hike_cell* front = (hike_cell*)calloc(count, sizeof(hike_cell));
    if (back == NULL || front == NULL) {
        free(back);
        free(front);
        return HIKE_ERR_OUT_OF_MEMORY;
    }

    ctx->back = back;
    ctx->front = front;
    ctx->w = w;
    ctx->h = h;

    hike_cell blank = blank_cell();
    for (size_t i = 0; i < count; ++i) ctx->back[i] = blank;
    /* The front buffer starts as zeroed cells, which is a state no draw can
     * produce -- a code point of zero is not a blank. That difference is
     * deliberate: it makes the first present repaint everything, rather than
     * trusting a screen we have never written to. */

    ctx->clip_top = 0;
    ctx->clips[0].x = 0;
    ctx->clips[0].y = 0;
    ctx->clips[0].w = w;
    ctx->clips[0].h = h;
    ctx->force_full_repaint = true;
    return HIKE_OK;
}

void hike_buffers_free(hike_context* ctx) {
    if (ctx == NULL) return;
    free(ctx->back);
    free(ctx->front);
    ctx->back = NULL;
    ctx->front = NULL;
}

/* --------------------------------------------------------------- draw ---- */

static hike_cell* cell_at(hike_context* ctx, int x, int y) {
    if (x < 0 || y < 0 || x >= ctx->w || y >= ctx->h) return NULL;
    return &ctx->back[(size_t)y * (size_t)ctx->w + (size_t)x];
}

/* Repairs the other half of any wide pair that cell (x, y) belongs to.
 *
 * Called before every write, and deliberately not clipped: the partner of a
 * cell inside the clip can be outside it, and leaving that partner behind
 * would leave the grid holding half a glyph. Clipping governs what a caller
 * may draw, not whether libhike may keep its own invariant. */
static void break_pair(hike_context* ctx, int x, int y) {
    hike_cell* c = cell_at(ctx, x, y);
    if (c == NULL) return;

    if (c->ch == HIKE_CELL_CONTINUATION) {
        hike_cell* lead = cell_at(ctx, x - 1, y);
        if (lead != NULL) lead->ch = ' ';
        return;
    }
    if (hike_char_width(c->ch) == 2) {
        hike_cell* tail = cell_at(ctx, x + 1, y);
        if (tail != NULL && tail->ch == HIKE_CELL_CONTINUATION) tail->ch = ' ';
    }
}

/* The single point through which every write to the back buffer passes. */
static void put(hike_context* ctx, int x, int y, hike_cell cell) {
    if (!hike_rect_contains(ctx->clips[ctx->clip_top], x, y)) return;
    hike_cell* dst = cell_at(ctx, x, y);
    if (dst == NULL) return;
    break_pair(ctx, x, y);
    *dst = cell;
}

void hike_clear(hike_context* ctx) {
    if (ctx == NULL || ctx->back == NULL) return;
    /* Clear ignores the clip stack on purpose. It means "the frame starts
     * here", and a clear that a stray unbalanced push could shrink would be a
     * confusing thing to debug. hike_fill is the clipped way to blank a
     * region. */
    hike_cell blank = blank_cell();
    size_t count = (size_t)ctx->w * (size_t)ctx->h;
    for (size_t i = 0; i < count; ++i) ctx->back[i] = blank;
}

void hike_set_cell(hike_context* ctx, int x, int y, hike_cell cell) {
    if (ctx == NULL || ctx->back == NULL) return;

    /* A caller cannot hand us a continuation directly. It is libhike's
     * bookkeeping, not a character, and accepting it would let a caller
     * manufacture the torn state the sentinel exists to prevent. */
    if (cell.ch == HIKE_CELL_CONTINUATION) return;

    if (hike_char_width(cell.ch) == 2) {
        /* Both halves have to land or neither should: a lead written at the
         * last column would have its continuation off the grid. */
        if (x + 1 >= ctx->w) return;
        if (!hike_rect_contains(ctx->clips[ctx->clip_top], x, y) ||
            !hike_rect_contains(ctx->clips[ctx->clip_top], x + 1, y))
            return;

        break_pair(ctx, x, y);
        break_pair(ctx, x + 1, y);

        hike_cell tail = cell;
        tail.ch = HIKE_CELL_CONTINUATION;
        *cell_at(ctx, x, y) = cell;
        *cell_at(ctx, x + 1, y) = tail;
        return;
    }

    put(ctx, x, y, cell);
}

hike_cell hike_get_cell(const hike_context* ctx, int x, int y) {
    if (ctx == NULL || ctx->back == NULL ||
        x < 0 || y < 0 || x >= ctx->w || y >= ctx->h)
        return blank_cell();
    return ctx->back[(size_t)y * (size_t)ctx->w + (size_t)x];
}

int hike_text(hike_context* ctx, int x, int y, const char* utf8,
              hike_color fg, hike_color bg, uint16_t attrs) {
    if (ctx == NULL || ctx->back == NULL || utf8 == NULL) return 0;

    size_t len = strlen(utf8);
    size_t i = 0;
    int columns = 0;

    while (i < len) {
        uint32_t cp = 0;
        size_t used = hike_utf8_decode(utf8 + i, len - i, &cp);
        if (used == 0) {
            /* The decoder refuses to guess, but something has to be drawn or
             * the rest of the line would silently shift. A replacement
             * character makes the bad byte visible where it occurred and costs
             * exactly the one column a caller's layout already assumed. */
            cp = 0xFFFD;
            used = 1;
        }

        int w = hike_char_width(cp);
        if (w == 0) {
            /* Combining marks and zero-width controls take no column. libhike
             * stores one code point per cell, so there is nowhere to put a
             * mark that belongs to the preceding character; dropping it is
             * wrong for the text but right for the grid, and it is the choice
             * that keeps every later column correct. */
            i += used;
            continue;
        }

        hike_cell cell;
        cell.ch = cp;
        cell.fg = fg;
        cell.bg = bg;
        cell.attrs = attrs;
        hike_set_cell(ctx, x + columns, y, cell);

        /* The advance counts what the text occupies, not what was drawn.
         * Clipped-away characters still take their columns, so a caller
         * laying out a line gets the same answer whatever it is drawing
         * inside. */
        columns += w;
        i += used;
    }
    return columns;
}

void hike_fill(hike_context* ctx, hike_rect r, hike_cell cell) {
    if (ctx == NULL || ctx->back == NULL) return;
    for (int yy = r.y; yy < r.y + r.h; ++yy)
        for (int xx = r.x; xx < r.x + r.w; ++xx)
            hike_set_cell(ctx, xx, yy, cell);
}

void hike_push_clip(hike_context* ctx, hike_rect r) {
    if (ctx == NULL) return;

    /* A child can only ever narrow what its parent allowed. Intersecting here
     * rather than replacing is what makes clipping compose: a widget passes
     * its own bounds without having to know its parent's. */
    hike_rect next = hike_rect_intersect(ctx->clips[ctx->clip_top], r);

    if (ctx->clip_top + 1 >= HIKE_CLIP_STACK_MAX) {
        /* Deeper than the stack allows. Narrowing the top in place keeps the
         * safe property -- nothing draws outside its parent -- at the cost of
         * a pop that will not restore correctly. The alternative, ignoring the
         * push, would let a widget draw over its container. */
        ctx->clips[ctx->clip_top] = next;
        return;
    }
    ctx->clips[++ctx->clip_top] = next;
}

void hike_pop_clip(hike_context* ctx) {
    if (ctx == NULL) return;
    /* Index 0 is the screen and is not the caller's to pop. An unbalanced pop
     * is a bug in the caller, but one that removed the screen clip would turn
     * into out-of-bounds writes everywhere afterwards. */
    if (ctx->clip_top > 0) --ctx->clip_top;
}

void hike_invalidate(hike_context* ctx) {
    if (ctx == NULL) return;
    ctx->force_full_repaint = true;
}
