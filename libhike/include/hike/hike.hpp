/* libhike -- the C++ layer.
 *
 * This wraps EXACTLY the C surface in hike.h and hike/widgets.h. There is no
 * private back door: every member below ends in a call to a public C function,
 * and no state is kept here that the C API cannot also reach. That constraint
 * is what keeps the C API first class -- the moment C++ can do something C
 * cannot, C becomes the subset and starts to rot.
 *
 * What it adds is the things C cannot express, and only those:
 *
 *   * RAII. A Context restores the terminal in its destructor, so an exception
 *     thrown anywhere inside a UI cannot leave the user in raw mode with no
 *     echo and no cursor. In C that is the caller's discipline; here it is the
 *     language's job.
 *   * Errors as errors. hike_init returns a status and an out-parameter; the
 *     constructor here throws hike::Error, so an unchecked failure is not
 *     something a caller can write by accident.
 *   * Types. std::string_view for text, a Color with named constructors, and
 *     std::function callbacks instead of a function pointer and a void*.
 *   * A builder style for widgets, because filling a struct field by field
 *     reads like configuration and a UI is written, not configured:
 *
 *         hike::button("Save").width(12).on_click([&] { save(); }).draw(ctx, r);
 *
 * Widgets here are still drawn, not retained: a builder is a description you
 * make, draw with, and let go of at the end of the frame. Holding one across
 * frames is fine and costs nothing, but nothing requires it, so there is no
 * second copy of the caller's model to keep in step.
 */
#ifndef HIKE_HPP
#define HIKE_HPP

#include "hike/hike.h"
#include "hike/widgets.h"

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hike {

/* ---------------------------------------------------------------- basics */

using Rect = hike_rect;
using Point = hike_point;
using Cell = hike_cell;
using Event = hike_event;
using Key = hike_key;
using Status = hike_status;
using Depth = hike_color_depth;
using Border = hike_border_style;
using Align = hike_align;

/* Thrown by anything that cannot report a failure in its return type -- which,
 * because of RAII, means constructors above all. */
class Error : public std::runtime_error {
public:
    explicit Error(Status status);
    Status status() const noexcept { return status_; }

private:
    Status status_;
};

/* A colour, with named constructors instead of a tag field the caller has to
 * set consistently with the union-like members beside it. */
class Color {
public:
    Color() : c_(hike_default_color()) {}
    Color(hike_color c) : c_(c) {}                     /* NOLINT: intentional */

    static Color rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        return Color(hike_rgb(r, g, b));
    }
    static Color indexed(std::uint8_t index) { return Color(hike_indexed(index)); }
    static Color terminal_default() { return Color(hike_default_color()); }

    operator hike_color() const { return c_; }         /* NOLINT: intentional */
    hike_color raw() const { return c_; }

private:
    hike_color c_;
};

/* A style, built by chaining, because the common case is one change from the
 * default and a struct makes you write the other four fields anyway. */
class Style {
public:
    Style() : s_(hike_style_default()) {}
    Style(hike_style s) : s_(s) {}                     /* NOLINT: intentional */

    Style& fg(Color c) { s_.fg = c; return *this; }
    Style& bg(Color c) { s_.bg = c; return *this; }
    Style& attrs(std::uint16_t a) { s_.attrs = a; return *this; }
    Style& add(std::uint16_t a) { s_.attrs = std::uint16_t(s_.attrs | a); return *this; }
    Style& bold() { return add(HIKE_BOLD); }
    Style& dim() { return add(HIKE_DIM); }
    Style& italic() { return add(HIKE_ITALIC); }
    Style& underline() { return add(HIKE_UNDERLINE); }
    Style& reverse() { return add(HIKE_REVERSE); }

    operator hike_style() const { return s_; }         /* NOLINT: intentional */
    hike_style raw() const { return s_; }

private:
    hike_style s_;
};

using Theme = hike_theme;
inline Theme default_theme() { return hike_default_theme(); }

/* --------------------------------------------------------------- context */

class Context;

/* A clip region held for a scope. The C API's clip stack is push/pop, and a
 * pop missed on an early return leaves every later draw wrongly constrained;
 * a scope guard makes that unwritable. */
class Clip {
public:
    Clip(Context& ctx, Rect r);
    ~Clip();
    Clip(const Clip&) = delete;
    Clip& operator=(const Clip&) = delete;

private:
    Context& ctx_;
};

class Options {
public:
    Options() : o_(hike_default_options()) {}

    Options& mouse(bool on = true) { o_.mouse = on; return *this; }
    Options& bracketed_paste(bool on = true) { o_.bracketed_paste = on; return *this; }
    Options& focus_events(bool on = true) { o_.focus_events = on; return *this; }
    Options& alternate_screen(bool on = true) { o_.alternate_screen = on; return *this; }
    Options& hide_cursor(bool on = true) { o_.hide_cursor = on; return *this; }

    const hike_options& raw() const { return o_; }

private:
    hike_options o_;
};

/* The session. Move-only: two objects that both shut the terminal down would
 * shut it down twice. */
class Context {
public:
    explicit Context(const Options& options = Options());

    /* Adopts an already-initialised C context, for a program that is mostly C
     * and wants the C++ layer for one screen. Ownership transfers: this will
     * shut it down. Offered because the alternative is that mixing the two
     * requires giving up RAII, which would make the wrapper's main advantage
     * conditional on never talking to C. */
    static Context adopt(hike_context* raw);

    ~Context();
    Context(Context&& other) noexcept;
    Context& operator=(Context&& other) noexcept;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    hike_context* raw() const { return ctx_; }

    int width() const;
    int height() const;
    Rect bounds() const;
    Depth depth() const;

    void clear();
    void set_cell(int x, int y, Cell cell);
    Cell cell(int x, int y) const;
    int text(int x, int y, std::string_view utf8, Style style = Style());
    void fill(Rect r, Cell cell);
    void present();
    void invalidate();
    void cursor(int x, int y, bool visible = true);

    void push_clip(Rect r);
    void pop_clip();

    /* std::nullopt where the C API returns false, so a caller cannot read the
     * event struct it did not receive. */
    std::optional<Event> poll(int timeout_ms = -1);

private:
    explicit Context(hike_context* raw) : ctx_(raw) {}
    hike_context* ctx_;
};

/* ---------------------------------------------------------------- layout */

using Size = hike_size;
inline Size fixed(int cells) { return hike_fixed(cells); }
inline Size content(int cells) { return hike_content(cells); }
inline Size weight(int w) { return hike_weight(w); }
/* Measures text the way the terminal will, in columns rather than bytes. */
inline Size content(std::string_view text);

class Layout {
public:
    static Layout row() { return Layout(hike_row()); }
    static Layout column() { return Layout(hike_column()); }

    Layout& gap(int cells) { l_.gap = cells; return *this; }
    Layout& pad(int all) { l_ = hike_layout_pad(l_, all); return *this; }
    Layout& pad(int horizontal, int vertical);
    Layout& pad(int left, int top, int right, int bottom);

    Layout& add(Size s) { sizes_.push_back(s); return *this; }
    Layout& fixed(int cells) { return add(hike::fixed(cells)); }
    Layout& content(int cells) { return add(hike::content(cells)); }
    Layout& content(std::string_view text) { return add(hike::content(text)); }
    Layout& weight(int w) { return add(hike::weight(w)); }

    /* Splits with the sizes added so far. */
    std::vector<Rect> split(Rect area) const;
    /* Splits with sizes given at the call site, for the one-liner case. */
    std::vector<Rect> split(Rect area, const std::vector<Size>& sizes) const;

    const hike_layout& raw() const { return l_; }

private:
    explicit Layout(hike_layout l) : l_(l) {}
    hike_layout l_;
    std::vector<Size> sizes_;
};

inline Layout row() { return Layout::row(); }
inline Layout column() { return Layout::column(); }

/* ---------------------------------------------------------------- focus */

/* The focus ring, with the same rule as the C layer: Tab and Shift+Tab move
 * it and are consumed; every other key belongs to the focused widget alone. */
class Focus {
public:
    explicit Focus(int count = 0) : f_(hike_focus_make(count)) {}

    int index() const { return f_.index; }
    int count() const { return f_.count; }
    bool has(int index) const { return hike_focus_has(&f_, index); }
    void set(int index) { hike_focus_set(&f_, index); }
    void next() { hike_focus_next(&f_); }
    void prev() { hike_focus_prev(&f_); }
    /* True when the ring took the event, in which case no widget may see it. */
    bool key(const Event& ev) { return hike_focus_key(&f_, &ev); }

private:
    hike_focus f_;
};

/* --------------------------------------------------------------- widgets */

/* The chaining below returns a reference rather than a copy, so a builder can
 * be configured over several statements and still be one object. The
 * free-function constructors (hike::button and friends) return by value, which
 * makes the one-liner form work as well. */

class Label {
public:
    explicit Label(std::string_view text) : text_(text) {}

    Label& align(Align a) { align_ = a; return *this; }
    Label& left() { return align(HIKE_ALIGN_LEFT); }
    Label& center() { return align(HIKE_ALIGN_CENTER); }
    Label& right() { return align(HIKE_ALIGN_RIGHT); }
    Label& style(Style s) { style_ = s; return *this; }

    int draw(Context& ctx, Rect r) const;

private:
    std::string text_;
    Align align_ = HIKE_ALIGN_LEFT;
    Style style_;
};

class Box {
public:
    Box() = default;
    explicit Box(std::string_view title) : title_(title) {}

    Box& title(std::string_view t) { title_ = t; return *this; }
    Box& border(Border b) { border_ = b; return *this; }
    Box& single() { return border(HIKE_BORDER_SINGLE); }
    Box& doubled() { return border(HIKE_BORDER_DOUBLE); }
    Box& rounded() { return border(HIKE_BORDER_ROUNDED); }
    Box& thick() { return border(HIKE_BORDER_THICK); }
    Box& ascii() { return border(HIKE_BORDER_ASCII); }
    Box& style(Style s) { style_ = s; return *this; }

    /* Returns the rect inside the frame, which is what the caller needs next. */
    Rect draw(Context& ctx, Rect r) const;

private:
    std::string title_;
    Border border_ = HIKE_BORDER_SINGLE;
    Style style_;
};

class Button {
public:
    explicit Button(std::string_view label) : label_(label) {}

    Button& width(int cells) { width_ = cells; return *this; }
    Button& align(Align a) { align_ = a; return *this; }
    Button& focused(bool on = true) { focused_ = on; return *this; }
    Button& disabled(bool on = true) { disabled_ = on; return *this; }
    Button& theme(Theme t) { theme_ = t; return *this; }
    Button& on_click(std::function<void()> fn) { on_click_ = std::move(fn); return *this; }

    void draw(Context& ctx, Rect r) const;
    /* True when the event activated the button. The callback, if any, has
     * already run by then; the return value is for a caller that would rather
     * write the effect at the call site than in a lambda. */
    bool dispatch(Rect r, const Event& ev) const;

    /* Columns this button wants: the width set, or the label plus its
     * brackets. Exposed so it can feed a content-sized layout child. */
    int measure() const;

private:
    hike_button make() const;

    std::string label_;
    int width_ = -1;
    Align align_ = HIKE_ALIGN_CENTER;
    bool focused_ = false;
    bool disabled_ = false;
    Theme theme_ = hike_default_theme();
    std::function<void()> on_click_;
};

class Checkbox {
public:
    explicit Checkbox(std::string_view label) : label_(label) {}

    Checkbox& checked(bool on = true) { checked_ = on; return *this; }
    Checkbox& focused(bool on = true) { focused_ = on; return *this; }
    Checkbox& disabled(bool on = true) { disabled_ = on; return *this; }
    Checkbox& theme(Theme t) { theme_ = t; return *this; }
    Checkbox& on_change(std::function<void(bool)> fn) { on_change_ = std::move(fn); return *this; }

    bool is_checked() const { return checked_; }
    void draw(Context& ctx, Rect r) const;
    bool dispatch(Rect r, const Event& ev);

private:
    hike_checkbox make() const;

    std::string label_;
    bool checked_ = false;
    bool focused_ = false;
    bool disabled_ = false;
    Theme theme_ = hike_default_theme();
    std::function<void(bool)> on_change_;
};

class RadioGroup {
public:
    RadioGroup() = default;
    explicit RadioGroup(std::vector<std::string> labels) : labels_(std::move(labels)) {}

    RadioGroup& item(std::string_view label) { labels_.emplace_back(label); return *this; }
    RadioGroup& selected(int index) { selected_ = index; return *this; }
    RadioGroup& focused(bool on = true) { focused_ = on; return *this; }
    RadioGroup& disabled(bool on = true) { disabled_ = on; return *this; }
    RadioGroup& theme(Theme t) { theme_ = t; return *this; }
    RadioGroup& on_change(std::function<void(int)> fn) { on_change_ = std::move(fn); return *this; }

    int selection() const { return selected_; }
    void draw(Context& ctx, Rect r) const;
    bool dispatch(Rect r, const Event& ev);

private:
    hike_radio_group make(std::vector<const char*>& storage) const;

    std::vector<std::string> labels_;
    int selected_ = 0;
    bool focused_ = false;
    bool disabled_ = false;
    Theme theme_ = hike_default_theme();
    std::function<void(int)> on_change_;
};

class List {
public:
    List() = default;
    explicit List(std::vector<std::string> items) : items_(std::move(items)) {}

    List& item(std::string_view text) { items_.emplace_back(text); return *this; }
    List& items(std::vector<std::string> v) { items_ = std::move(v); return *this; }
    List& selected(int index) { selected_ = index; return *this; }
    List& scroll(int top) { scroll_ = top; return *this; }
    List& focused(bool on = true) { focused_ = on; return *this; }
    List& theme(Theme t) { theme_ = t; return *this; }
    List& on_select(std::function<void(int)> fn) { on_select_ = std::move(fn); return *this; }

    int selection() const { return selected_; }
    int scroll_top() const { return scroll_; }
    int size() const { return int(items_.size()); }

    void draw(Context& ctx, Rect r);
    bool dispatch(int height, const Event& ev);

private:
    hike_list make(std::vector<const char*>& storage) const;
    void take(const hike_list& l);

    std::vector<std::string> items_;
    int selected_ = 0;
    int scroll_ = 0;
    bool focused_ = false;
    Theme theme_ = hike_default_theme();
    std::function<void(int)> on_select_;
};

/* A text input that owns its buffer, so a caller does not have to keep a char
 * array alive alongside it. The C widget's buffer is the caller's on purpose;
 * here the caller is this object. */
class Input {
public:
    explicit Input(std::size_t capacity = 256);

    Input& text(std::string_view value);
    Input& placeholder(std::string_view value) { placeholder_ = value; return *this; }
    Input& masked(bool on = true) { masked_ = on; return *this; }
    Input& focused(bool on = true) { focused_ = on; return *this; }
    Input& theme(Theme t) { theme_ = t; return *this; }
    Input& on_change(std::function<void(std::string_view)> fn) { on_change_ = std::move(fn); return *this; }

    std::string_view value() const;
    std::size_t cursor() const { return in_.cursor; }
    int cursor_column() const { return hike_input_cursor_column(&in_); }
    int scroll() const { return in_.scroll; }

    void draw(Context& ctx, Rect r);
    bool dispatch(int width, const Event& ev);

private:
    void sync();

    std::vector<char> buf_;
    hike_input in_{};
    std::string placeholder_;
    bool masked_ = false;
    bool focused_ = false;
    Theme theme_ = hike_default_theme();
    std::function<void(std::string_view)> on_change_;
};

class Progress {
public:
    explicit Progress(double value = 0.0) : value_(value) {}

    Progress& value(double v) { value_ = v; return *this; }
    Progress& percent(bool on = true) { percent_ = on; return *this; }
    Progress& theme(Theme t) { theme_ = t; return *this; }

    void draw(Context& ctx, Rect r) const;

private:
    double value_;
    bool percent_ = false;
    Theme theme_ = hike_default_theme();
};

class TextView {
public:
    TextView() = default;
    explicit TextView(std::vector<std::string> lines) : lines_(std::move(lines)) {}

    TextView& line(std::string_view text) { lines_.emplace_back(text); return *this; }
    TextView& lines(std::vector<std::string> v) { lines_ = std::move(v); return *this; }
    TextView& scroll(int top) { scroll_ = top; return *this; }
    TextView& hscroll(int left) { hscroll_ = left; return *this; }
    TextView& focused(bool on = true) { focused_ = on; return *this; }
    TextView& theme(Theme t) { theme_ = t; return *this; }

    int scroll_top() const { return scroll_; }
    int scroll_left() const { return hscroll_; }

    void draw(Context& ctx, Rect r);
    bool dispatch(int height, const Event& ev);

private:
    hike_textview make(std::vector<const char*>& storage) const;

    std::vector<std::string> lines_;
    int scroll_ = 0;
    int hscroll_ = 0;
    bool focused_ = false;
    Theme theme_ = hike_default_theme();
};

class Tabs {
public:
    Tabs() = default;
    explicit Tabs(std::vector<std::string> labels) : labels_(std::move(labels)) {}

    Tabs& tab(std::string_view label) { labels_.emplace_back(label); return *this; }
    Tabs& selected(int index) { selected_ = index; return *this; }
    Tabs& focused(bool on = true) { focused_ = on; return *this; }
    Tabs& theme(Theme t) { theme_ = t; return *this; }
    Tabs& on_change(std::function<void(int)> fn) { on_change_ = std::move(fn); return *this; }

    int selection() const { return selected_; }
    void draw(Context& ctx, Rect r) const;
    bool dispatch(Rect r, const Event& ev);

private:
    hike_tabs make(std::vector<const char*>& storage) const;

    std::vector<std::string> labels_;
    int selected_ = 0;
    bool focused_ = false;
    Theme theme_ = hike_default_theme();
    std::function<void(int)> on_change_;
};

/* The one-liner constructors. These exist so a widget reads as a call rather
 * than a declaration: hike::button("Save").width(12) instead of a named
 * Button object that is used once. */
inline Label label(std::string_view text) { return Label(text); }
inline Box box(std::string_view title = {}) { return Box(title); }
inline Button button(std::string_view label) { return Button(label); }
inline Checkbox checkbox(std::string_view label) { return Checkbox(label); }
inline RadioGroup radio() { return RadioGroup(); }
inline List list() { return List(); }
inline Progress progress(double value = 0.0) { return Progress(value); }
inline TextView text_view() { return TextView(); }
inline Tabs tabs() { return Tabs(); }

/* --------------------------------------------------------------- utility */

/* Columns a string occupies once printed, not bytes and not code points. */
int text_width(std::string_view utf8);
inline int char_width(char32_t cp) { return hike_char_width(std::uint32_t(cp)); }

inline Size content(std::string_view text) { return hike_content(text_width(text)); }

inline bool contains(Rect r, int x, int y) { return hike_rect_contains(r, x, y); }
inline Rect intersect(Rect a, Rect b) { return hike_rect_intersect(a, b); }
inline Rect inset(Rect r, int amount) { return hike_rect_inset(r, amount); }

} // namespace hike

#endif /* HIKE_HPP */
