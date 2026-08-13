/* libhike internals, shared between the translation units that make up the
 * library. Nothing here is installed and nothing here is part of the contract
 * in hike/hike.h -- a caller that reaches in has no promise of stability.
 *
 * The one thing this header exists to make possible, beyond letting the
 * library's own files see the context, is testing. hike_init needs a real
 * terminal and refuses without one, so nothing that goes through it can run in
 * CI. hike_context_new_memory builds the same context over an in-memory sink
 * instead, which means the diff and the exact bytes a frame emits can be
 * asserted rather than eyeballed. The drawing and diffing code cannot tell the
 * difference: it only ever calls hike_out.
 */
#ifndef HIKE_INTERNAL_H
#define HIKE_INTERNAL_H

#include "hike/hike.h"

#ifndef _WIN32
#include <termios.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Clipping nests as deep as a widget tree does, and widget trees in a terminal
 * are shallow. A fixed stack keeps push_clip allocation-free, which matters
 * because it runs per widget per frame; overflow is clamped rather than
 * reported, since the alternative is a draw call that silently escapes its
 * container. */
#define HIKE_CLIP_STACK_MAX 32

/* The second cell of a wide character.
 *
 * A wide glyph occupies two columns but is one character, and the terminal
 * draws it when the first column is written. If the second cell held a real
 * code point the diff could decide only that cell changed, move the cursor
 * there and write it, which splits the glyph and desynchronises every column
 * after it on that row. So the second cell holds this sentinel instead: it is
 * not a valid Unicode scalar value, so it can never collide with real content,
 * and the present loop never emits it -- it emits the lead cell, which
 * advances the terminal's cursor over both columns by itself.
 *
 * Writing to either half of a pair repairs the other half to a blank, so the
 * grid never contains a lead without its continuation or the reverse. */
#define HIKE_CELL_CONTINUATION 0xFFFFFFFFu

struct hike_context {
    int w, h;

    /* back is what the caller draws into, front is what the terminal is
     * believed to be showing. present reconciles the two and then copies. */
    hike_cell* back;
    hike_cell* front;

    hike_rect clips[HIKE_CLIP_STACK_MAX];
    int clip_top; /* index of the active clip; 0 is the whole screen */

    bool force_full_repaint;

    int cursor_x, cursor_y;
    bool cursor_visible;
    /* What the terminal was last told, so present can stay silent when the
     * cursor has not moved. */
    int shown_cursor_x, shown_cursor_y;
    bool shown_cursor_visible;

    hike_color_depth depth;
    hike_options options;

    /* Output. Exactly one of these is live: a real terminal has out_fd >= 0,
     * a test context has out_fd < 0 and appends to sink instead. */
    int out_fd;
    char* sink;
    size_t sink_len, sink_cap;

    bool raw_active;
#ifndef _WIN32
    struct termios saved_termios;
#else
    unsigned long saved_out_mode, saved_in_mode;
#endif

    /* Owned entirely by the input layer, which is free to define whatever it
     * needs behind it. Kept as a void* so this header does not have to know
     * the shape of a decoder it does not maintain. */
    void* input_state;
};

/* Writes to whichever sink this context has. Never partial: a short write on a
 * real fd is retried, because half an escape sequence on the wire corrupts
 * everything after it. */
void hike_out(hike_context* ctx, const char* bytes, size_t len);

/* Builds a context with no terminal behind it. Used by the tests, and by
 * anything that wants to render a frame to bytes. Returns NULL on allocation
 * failure or a non-positive size. */
hike_context* hike_context_new_memory(int w, int h, hike_color_depth depth);

/* The bytes a memory context has accumulated, and a way to drop them between
 * frames so one frame's output can be asserted on its own. */
const char* hike_sink_data(const hike_context* ctx, size_t* len_out);
void hike_sink_clear(hike_context* ctx);

/* Allocation and teardown of the grid, shared by the real and memory paths. */
hike_status hike_buffers_alloc(hike_context* ctx, int w, int h);
void hike_buffers_free(hike_context* ctx);
void hike_context_free(hike_context* ctx);

/* Colour reduction. Exposed because it is the part of the terminal layer with
 * real arithmetic in it, and the part a UI depends on for legibility on a
 * terminal poorer than the one it was written against. */
uint8_t hike_color_to_256(hike_color c);
uint8_t hike_color_to_16(hike_color c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HIKE_INTERNAL_H */
