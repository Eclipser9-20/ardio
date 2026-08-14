/* The same screen as hello.cpp, written against the C API.
 *
 *     clang hello.c -lhike -o hello
 *
 * This is immediate mode: nothing is stored between frames. Every loop you
 * work out the rectangles and call a draw function for each widget, and when
 * the frame is over libhike remembers only the cells, not the widgets. State
 * that has to survive -- what is focused, what a text field contains -- lives
 * in your own variables, which is why they are declared outside the loop.
 *
 * The C++ layer builds a tree of widget values instead and walks it for you.
 * That is the declarative style, and it is a wrapper over exactly these calls.
 */
#include <hike/hike.h>
#include <hike/widgets.h>
#include <stdio.h>

int main(void) {
    hike_context* ctx = NULL;
    hike_options options = hike_default_options();
    hike_status status = hike_init(&ctx, &options);
    if (status != HIKE_OK) {
        fprintf(stderr, "hike: %s\n", hike_status_text(status));
        return 1;
    }

    /* State libhike does not keep for you. */
    hike_focus focus = hike_focus_make(1);
    int presses = 0;
    int running = 1;

    while (running) {
        hike_clear(ctx);

        hike_rect screen = {0, 0, hike_width(ctx), hike_height(ctx)};

        /* Split the screen top to bottom: a fixed header, the body, a fixed
         * footer. The sizes and the rectangles are two parallel arrays, which
         * is the part the C++ layer exists to hide. */
        hike_size rows[] = {hike_fixed(3), hike_weight(1), hike_fixed(1)};
        hike_rect area[3];
        hike_layout column = hike_column();
        column.gap = 1;
        hike_layout_split(column, screen, rows, 3, area);

        /* Header: a titled box with a label inside its inner rect. */
        hike_rect inner = hike_box(ctx, area[0], HIKE_BORDER_SINGLE, "libhike",
                                   hike_style_default());
        hike_label(ctx, inner, "hello from a terminal", HIKE_ALIGN_LEFT,
                   hike_style_default());

        /* Body: two columns side by side. */
        hike_size cols[] = {hike_weight(1), hike_weight(1)};
        hike_rect body[2];
        hike_layout side = hike_row();
        side.gap = 1;
        hike_layout_split(side, area[1], cols, 2, body);

        hike_button press = hike_button_make("press me");
        press.focused = hike_focus_has(&focus, 0);
        hike_button_draw(ctx, body[0], &press);

        char counted[64];
        snprintf(counted, sizeof counted, "pressed %d times", presses);
        hike_label(ctx, body[1], counted, HIKE_ALIGN_LEFT, hike_style_default());

        hike_label(ctx, area[2], "enter presses, q quits", HIKE_ALIGN_LEFT,
                   hike_style_default());

        /* Nothing has reached the terminal until now. */
        hike_present(ctx);

        hike_event event;
        if (hike_poll(ctx, &event, -1)) {
            if (event.kind == HIKE_EVENT_KEY) {
                if (event.key.ch == 'q') running = 0;
                if (event.key.key == HIKE_KEY_ENTER) presses++;
            }
        }
    }

    /* Puts the terminal back. Skipping this leaves an unusable shell. */
    hike_shutdown(ctx);
    return 0;
}
