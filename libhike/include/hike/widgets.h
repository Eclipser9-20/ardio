/* libhike -- layout and widgets.
 *
 * This is the layer a user actually writes against. It is built strictly on
 * top of hike.h and reaches into nothing private: everything here draws by
 * calling hike_set_cell, hike_text and hike_fill, and takes input as a
 * hike_event. A program that wants a widget libhike does not have can write it
 * the same way these are written, with no loss of capability, which is the
 * only real test of whether a widget layer is a library or a wall.
 *
 * Widgets are drawn, not retained. There is no tree, no invalidation and no
 * ownership of your data: you keep the state -- the text in an input, the
 * index in a list -- and hand it to a draw call each frame. The reason is that
 * a terminal UI redraws whole frames anyway (the core diffs cells for you), so
 * a retained tree buys nothing and costs the user a second copy of their model
 * that can disagree with the first.
 *
 * Layout is likewise a function, not an object: you give it a rect and a list
 * of sizes and it gives you back rects. That makes it testable and, more
 * importantly, explainable -- a user who cannot work out why a box came out
 * one column wide will stop trusting the library, so the rules below are few
 * and stated exactly.
 */
#ifndef HIKE_WIDGETS_H
#define HIKE_WIDGETS_H

#include "hike/hike.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- style */

/* A foreground, a background and attributes: the whole of what a cell can
 * carry. Widgets take styles by value rather than consulting a global theme,
 * because a global theme is a hidden argument and makes two identical-looking
 * calls behave differently. */
typedef struct {
    hike_color fg;
    hike_color bg;
    uint16_t attrs;
} hike_style;

hike_style hike_style_default(void);
hike_style hike_style_make(hike_color fg, hike_color bg, uint16_t attrs);

/* The styles a widget needs to show its state. Passing one struct keeps the
 * call sites short, and a caller who only cares about the focused colour can
 * start from hike_default_theme and change the one field. */
typedef struct {
    hike_style normal;
    hike_style focused;   /* this widget holds the focus */
    hike_style selected;  /* the selected row of a list, the checked radio */
    hike_style disabled;
    hike_style accent;    /* borders, the filled part of a progress bar */
} hike_theme;

hike_theme hike_default_theme(void);

typedef enum {
    HIKE_ALIGN_LEFT = 0,
    HIKE_ALIGN_CENTER,
    HIKE_ALIGN_RIGHT
} hike_align;

/* --------------------------------------------------------------- layout */

/* How one child asks for space along the layout's axis.
 *
 *   FIXED    exactly `value` cells.
 *   CONTENT  exactly `value` cells, where value is the size the caller
 *            measured from its own content (hike_text_width, a row count).
 *            It is separate from FIXED only so that reading the call site
 *            tells you where the number came from; they resolve identically.
 *   WEIGHT   a share of whatever is left, in proportion to `value`.
 */
typedef enum {
    HIKE_SIZE_FIXED = 0,
    HIKE_SIZE_CONTENT,
    HIKE_SIZE_WEIGHT
} hike_size_kind;

typedef struct {
    hike_size_kind kind;
    int value;
} hike_size;

hike_size hike_fixed(int cells);
hike_size hike_content(int cells);
hike_size hike_weight(int weight);

typedef enum {
    HIKE_LAYOUT_ROW = 0,   /* children side by side, splitting the width */
    HIKE_LAYOUT_COLUMN     /* children stacked, splitting the height */
} hike_layout_dir;

typedef struct {
    hike_layout_dir dir;
    int pad_left, pad_top, pad_right, pad_bottom;
    int gap;               /* blank cells between adjacent children */
} hike_layout;

hike_layout hike_row(void);
hike_layout hike_column(void);
hike_layout hike_layout_pad(hike_layout l, int all);

/* Splits `area` among `n` children, writing their rects to `out`.
 *
 * The algorithm, in full, because a layout engine nobody can predict is worse
 * than no layout engine:
 *
 *   1. Padding comes off `area` first. What remains is the inner rect, never
 *      smaller than zero in either dimension.
 *   2. Gaps come off next: (n - 1) * gap cells, or as many as exist.
 *   3. FIXED and CONTENT children are served in declaration order, each taking
 *      min(value, what is still unclaimed). So when the space runs out it is
 *      the LAST children that get nothing, and the first ones are unaffected.
 *      Shrinking everybody a little instead would make every child's size
 *      depend on every other child's, which is exactly the situation where a
 *      user cannot explain a result.
 *   4. What is left is split among the WEIGHT children: child i gets
 *      floor(left * weight_i / total_weight), and the cells lost to that floor
 *      -- there are always fewer of them than there are weighted children --
 *      are handed out one each, again in declaration order. The sizes
 *      therefore sum to exactly the space available, and the earlier child is
 *      the wider one when a split does not divide evenly. 10 cells across
 *      three equal weights is 4, 3, 3.
 *   5. If there are no weighted children the leftover is simply not used; it
 *      is left at the end of the row. Silently stretching the last child would
 *      make a FIXED size not fixed.
 *
 * On the cross axis every child gets the full inner extent.
 *
 * Returns the number of rects written, which is `n`, or 0 if the arguments are
 * unusable (null pointers, negative n). Zero-sized children are still written
 * out with their correct position, so a caller can lay out and then skip the
 * empty ones rather than having the indices shift underneath it. */
int hike_layout_split(hike_layout layout, hike_rect area,
                      const hike_size* sizes, int n, hike_rect* out);

/* Insets a rect on all sides, clamped so the result is never negative. */
hike_rect hike_rect_inset(hike_rect r, int amount);

/* --------------------------------------------------------------- focus */

/* The focus ring.
 *
 * THE RULE, stated once and applied everywhere:
 *
 *   A key event is offered to the focus ring first, and the ring consumes Tab
 *   and Shift+Tab and nothing else. Every other key event goes to exactly one
 *   widget -- the focused one -- and to no other. There is no bubbling, no
 *   capture phase and no global accelerator table. A widget that is not
 *   focused never sees a key.
 *
 * Mouse events are the exception, and deliberately: a click is positional, so
 * it goes to whatever is under the pointer, and a widget that handles one
 * takes the focus as a result.
 *
 * The ring is a count and an index rather than a list of widgets, because the
 * widgets here are drawn rather than retained and there is nothing to hold. A
 * caller compares the index against its own numbering. */
typedef struct {
    int count;     /* how many focusable widgets exist this frame */
    int index;     /* which one has the focus; clamped into range on use */
} hike_focus;

hike_focus hike_focus_make(int count);
bool hike_focus_has(const hike_focus* f, int index);
void hike_focus_next(hike_focus* f);
void hike_focus_prev(hike_focus* f);
void hike_focus_set(hike_focus* f, int index);

/* Consumes Tab and Shift+Tab, wrapping at both ends. Returns true when it took
 * the event, in which case the caller must not pass it to a widget. */
bool hike_focus_key(hike_focus* f, const hike_event* ev);

/* --------------------------------------------------------------- widgets */

/* Draws text in a rect's first line, clipped to the rect, aligned. Returns the
 * columns drawn. Text too wide for the rect is cut, not wrapped: a label is
 * one line by definition, and silently growing to two would push whatever the
 * caller put underneath it off the screen. */
int hike_label(hike_context* ctx, hike_rect r, const char* utf8,
               hike_align align, hike_style style);

typedef enum {
    HIKE_BORDER_NONE = 0,
    HIKE_BORDER_SINGLE,   /* the box-drawing set: light lines */
    HIKE_BORDER_DOUBLE,
    HIKE_BORDER_ROUNDED,  /* single, with arcs at the corners */
    HIKE_BORDER_THICK,
    HIKE_BORDER_ASCII     /* +-| , for terminals or fonts that cannot do better */
} hike_border_style;

/* Draws a frame with an optional title, and returns the rect INSIDE it, which
 * is what the caller wants next in every case. A rect too small to hold a
 * border draws what fits and returns an empty inner rect. */
hike_rect hike_box(hike_context* ctx, hike_rect r, hike_border_style border,
                   const char* title, hike_style style);

/* A button. `focused` decides both the drawn style and, by the focus rule,
 * whether a key can activate it. Returns true when this event activated it:
 * Enter or Space when focused, or a press inside the rect. */
typedef struct {
    const char* label;
    hike_align align;
    bool focused;
    bool disabled;
    hike_theme theme;
} hike_button;

hike_button hike_button_make(const char* label);
void hike_button_draw(hike_context* ctx, hike_rect r, const hike_button* b);
bool hike_button_event(const hike_button* b, hike_rect r, const hike_event* ev);

/* A single-line text input.
 *
 * The caller owns the buffer, so an input can edit a field of the caller's own
 * struct with no copy and no allocation inside libhike. `cap` is the size of
 * that buffer in bytes including the terminator.
 *
 * `cursor` is a BYTE offset into the text, not a column: a cursor stored as a
 * column cannot describe a position inside a multi-byte character, and every
 * edit would have to re-scan to find out what it meant. Columns are derived
 * when drawing. `scroll` is a column, because that is what it is used for. */
typedef struct {
    char* buf;
    size_t cap;
    size_t len;
    size_t cursor;
    int scroll;
    bool focused;
    bool masked;        /* draw asterisks; for passwords */
    const char* placeholder;
    hike_theme theme;
} hike_input;

hike_input hike_input_make(char* buf, size_t cap);
void hike_input_set_text(hike_input* in, const char* utf8);

/* Draws the field, scrolling horizontally so the cursor is always visible, and
 * places the terminal cursor when focused. */
void hike_input_draw(hike_context* ctx, hike_rect r, hike_input* in);

/* Handles one event. Returns true when it consumed it. The width matters
 * because moving the cursor is what scrolls the view, so the input has to know
 * how wide it was drawn; pass the same rect width used for drawing. */
bool hike_input_event(hike_input* in, int width, const hike_event* ev);

/* The column the cursor sits at within the field, after scrolling. Exposed
 * because a test, or a caller drawing its own cursor, has no other way to ask,
 * and a caller that cannot ask would have to re-derive the scrolling rule. */
int hike_input_cursor_column(const hike_input* in);

typedef struct {
    const char* label;
    bool checked;
    bool focused;
    bool disabled;
    hike_theme theme;
} hike_checkbox;

hike_checkbox hike_checkbox_make(const char* label, bool checked);
void hike_checkbox_draw(hike_context* ctx, hike_rect r, const hike_checkbox* c);
/* Toggles on Space or Enter when focused, or on a click. Returns true when the
 * checked state changed. */
bool hike_checkbox_event(hike_checkbox* c, hike_rect r, const hike_event* ev);

typedef struct {
    const char* const* labels;
    int count;
    int selected;
    bool focused;
    bool disabled;
    hike_theme theme;
} hike_radio_group;

hike_radio_group hike_radio_make(const char* const* labels, int count, int selected);
void hike_radio_draw(hike_context* ctx, hike_rect r, const hike_radio_group* g);
/* Up/Down (and Left/Right) move the selection, without wrapping: a radio group
 * is a short list the user is reading, and wrapping past the end of one reads
 * as the selection jumping for no reason. Returns true when it changed. */
bool hike_radio_event(hike_radio_group* g, hike_rect r, const hike_event* ev);

/* A vertical list with a selection and a scroll offset. `scroll` is the index
 * of the first visible row and is adjusted on draw so the selection is always
 * on screen -- doing it on draw rather than on key means a selection changed
 * by the program, not the user, scrolls into view too. */
typedef struct {
    const char* const* items;
    int count;
    int selected;
    int scroll;
    bool focused;
    hike_theme theme;
} hike_list;

hike_list hike_list_make(const char* const* items, int count);
void hike_list_draw(hike_context* ctx, hike_rect r, hike_list* l);
bool hike_list_event(hike_list* l, int height, const hike_event* ev);

typedef struct {
    double value;          /* clamped to 0..1 */
    bool show_percent;
    hike_theme theme;
} hike_progress;

hike_progress hike_progress_make(double value);
void hike_progress_draw(hike_context* ctx, hike_rect r, const hike_progress* p);

/* A scrollable view over pre-split lines. It takes lines rather than a blob
 * because wrapping is a policy the caller has to choose anyway, and a view
 * that wrapped for you could not show a log file with long lines intact. */
typedef struct {
    const char* const* lines;
    int count;
    int scroll;            /* first visible line */
    int hscroll;           /* first visible column */
    bool focused;
    hike_theme theme;
} hike_textview;

hike_textview hike_textview_make(const char* const* lines, int count);
void hike_textview_draw(hike_context* ctx, hike_rect r, hike_textview* v);
bool hike_textview_event(hike_textview* v, int height, const hike_event* ev);

/* A tab strip. Draws only the labels; the caller draws the body of the
 * selected tab into whatever rect it likes. */
typedef struct {
    const char* const* labels;
    int count;
    int selected;
    bool focused;
    hike_theme theme;
} hike_tabs;

hike_tabs hike_tabs_make(const char* const* labels, int count);
void hike_tabs_draw(hike_context* ctx, hike_rect r, const hike_tabs* t);
bool hike_tabs_event(hike_tabs* t, hike_rect r, const hike_event* ev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HIKE_WIDGETS_H */
