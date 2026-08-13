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
 *   * Widgets as VALUES, which is the thing C genuinely cannot have. In C a
 *     widget is a draw call, so a container cannot hold one and structure has
 *     to be expressed by the caller matching a list of rects against a list of
 *     draw calls by position. Here a widget is an object, a container owns its
 *     children by value, and the shape of the screen is the shape of the code:
 *
 *         auto ui = hike::row(
 *             hike::fixed(20, hike::list().item("one").item("two")),
 *             hike::weight(1, hike::button("Save").on_click(save))
 *         ).gap(1);
 *
 *         ui.draw(ctx, area, focus);
 *
 *     Note what is NOT here: no chain method appends a child. gap() and pad()
 *     configure the container and nothing else, children are arguments, and
 *     the two can never be confused for one another. An earlier version let
 *     both be chained off the same object, and it read as though .gap(1) and
 *     .weight(1) were the same kind of thing when one is configuration whose
 *     position is irrelevant and the other is a child whose position decides
 *     the layout.
 *
 * The tree is retained; the drawing underneath it is not. A draw call walks
 * the tree and makes exactly the public C calls a hand-written C program would
 * make, in the same order. Nothing is cached between frames except the rects
 * the last layout produced, which is what lets an event find the widget the
 * user clicked on.
 *
 * Lifetime: children are owned BY VALUE. hike::fixed(10, hike::button("Save"))
 * copies or moves the button into the tree there and then, so a temporary
 * argument is safe. The only references anywhere in this file are function
 * parameters, which live for the duration of the call and are copied from
 * before it returns.
 */
#ifndef HIKE_HPP
#define HIKE_HPP

#include "hike/hike.h"
#include "hike/widgets.h"

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
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
using Size = hike_size;

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
 * a scope guard makes that unwritable. Containers use it to constrain their
 * children, which is why a widget in a rect too small for it is cut off rather
 * than drawing over its neighbour. */
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

/* ---------------------------------------------------------------- focus */

/* The focus ring, with the same rule as the C layer, now enforced in one place
 * rather than by the caller at every widget:
 *
 *   A key event goes to the ring first. The ring consumes Tab and Shift+Tab
 *   and nothing else. Every other key goes to exactly one widget -- the
 *   focused one -- and to no other. A mouse event is positional and is offered
 *   to every widget under the pointer instead; a widget that takes one also
 *   takes the focus.
 *
 * The index is assigned by traversal order of the tree, so a caller no longer
 * counts its widgets by hand and keeps the numbers in step with the layout.
 * The count is recomputed from the tree on every draw, which means adding a
 * widget cannot leave a stale total behind. */
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

    /* Sets how many focusable widgets there are, keeping the current index if
     * it still exists and clamping it if it does not. Called by a container's
     * draw with the number found in the tree. */
    void resize(int count);

private:
    hike_focus f_;
};

/* --------------------------------------------------------------- widgets */

/* Leaf widgets. Each is a value: copyable, holding its own text, and drawn by
 * calling the matching C widget. The chaining setters configure one object and
 * always return a reference to it, so the whole of a widget's configuration is
 * one expression and none of it appends anything to anything else.
 *
 * Every widget that can hold the focus has a focused() setter and a
 * dispatch(Rect, const Event&). Those two members are how the tree recognises
 * a focusable widget -- there is no registration and no base class, so a
 * widget a user writes themselves joins the tree by having them. */

class Label {
public:
    explicit Label(std::string_view text) : text_(text) {}

    Label& align(Align a) { align_ = a; return *this; }
    Label& left() { return align(HIKE_ALIGN_LEFT); }
    Label& center() { return align(HIKE_ALIGN_CENTER); }
    Label& right() { return align(HIKE_ALIGN_RIGHT); }
    Label& style(Style s) { style_ = s; return *this; }

    /* Columns the text occupies, for a content-sized child. */
    int measure() const;

    int draw(Context& ctx, Rect r) const;

private:
    std::string text_;
    Align align_ = HIKE_ALIGN_LEFT;
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

    /* Columns this button wants: the width set, or the label plus its
     * brackets. Feeds a content-sized child. */
    int measure() const;

    void draw(Context& ctx, Rect r) const;
    /* True when the event activated the button. The callback, if any, has
     * already run by then; the return value is for a caller that would rather
     * write the effect at the call site than in a lambda. */
    bool dispatch(Rect r, const Event& ev) const;

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
    int measure() const;
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
    /* A list scrolls vertically, so what it needs from its rect is the height.
     * Both forms exist because the tree calls the rect one uniformly and a
     * caller driving a list by hand has only a height to give. */
    bool dispatch(int height, const Event& ev);
    bool dispatch(Rect r, const Event& ev) { return dispatch(r.h, ev); }

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
 * here the caller is this object.
 *
 * Copying needs care and gets it below: the C struct holds a pointer into this
 * object's own vector, so a copy whose pointer still referred to the original's
 * storage would read the wrong text and, once the original died, freed memory.
 * The copy and move members re-point it. */
class Input {
public:
    explicit Input(std::size_t capacity = 256);
    Input(const Input& other);
    Input(Input&& other) noexcept;
    Input& operator=(const Input& other);
    Input& operator=(Input&& other) noexcept;
    ~Input() = default;

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
    /* An input scrolls horizontally, so what it needs is the width. */
    bool dispatch(int width, const Event& ev);
    bool dispatch(Rect r, const Event& ev) { return dispatch(r.w, ev); }

private:
    void sync();
    void repoint();

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
    bool dispatch(Rect r, const Event& ev) { return dispatch(r.h, ev); }

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
    int measure() const;
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

/* ----------------------------------------------------------------- node */

namespace detail {

/* A widget is focusable if it can be told it has the focus and can be offered
 * an event for a rect. Detected on the members rather than declared by
 * inheriting from something, so a widget written outside this header joins the
 * tree by having the right shape and needs nothing from us. */
template <class T, class = void>
struct is_focusable : std::false_type {};
template <class T>
struct is_focusable<T, std::void_t<
    decltype(std::declval<T&>().focused(true)),
    decltype(std::declval<T&>().dispatch(Rect{}, std::declval<const Event&>()))>>
    : std::true_type {};

/* A container is anything that can report how many focusable widgets it holds;
 * it then also knows how to walk them. */
template <class T, class = void>
struct is_container : std::false_type {};
template <class T>
struct is_container<T, std::void_t<decltype(std::declval<const T&>().focus_count())>>
    : std::true_type {};

template <class T, class = void>
struct has_measure : std::false_type {};
template <class T>
struct has_measure<T, std::void_t<decltype(std::declval<const T&>().measure())>>
    : std::true_type {};

} // namespace detail

/* Any drawable, held by value.
 *
 * This is what makes a layout able to contain a widget at all. C cannot have
 * it -- a C widget is a call, not a thing -- and it is the one place where the
 * C++ layer keeps state the C API does not: the tree. That is legitimate
 * precisely because the tree does nothing at draw time except make the same
 * public C calls in the same order a C program would.
 *
 * Copyable, by cloning what it holds, so a tree can be assigned and passed
 * around like any other value. Copying a UI description is cheap and happens
 * once when it is built. */
class Node {
public:
    Node() = default;

    template <class T, class D = std::decay_t<T>,
              class = std::enable_if_t<!std::is_same_v<D, Node>>>
    Node(T&& widget)                                    /* NOLINT: intentional */
        : model_(std::make_unique<Model<D>>(std::forward<T>(widget))) {}

    Node(const Node& other) : model_(other.model_ ? other.model_->clone() : nullptr) {}
    Node(Node&&) noexcept = default;
    Node& operator=(const Node& other);
    Node& operator=(Node&&) noexcept = default;
    ~Node() = default;

    bool empty() const { return model_ == nullptr; }

    void draw(Context& ctx, Rect r) { if (model_) model_->draw(ctx, r); }
    int focus_count() const { return model_ ? model_->focus_count() : 0; }
    void apply_focus(int& next, int active) { if (model_) model_->apply_focus(next, active); }
    /* Offers an event to this subtree. `next` counts focusable widgets in
     * traversal order as it goes, `active` is the focused index, and a mouse
     * event that lands writes the index of the widget it landed on to `hit` so
     * a click can move the focus. */
    bool dispatch(const Event& ev, Rect r, int& next, int active, int* hit) {
        return model_ ? model_->dispatch(ev, r, next, active, hit) : false;
    }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual std::unique_ptr<Concept> clone() const = 0;
        virtual void draw(Context&, Rect) = 0;
        virtual int focus_count() const = 0;
        virtual void apply_focus(int& next, int active) = 0;
        virtual bool dispatch(const Event&, Rect, int& next, int active, int* hit) = 0;
    };

    template <class T>
    struct Model final : Concept {
        explicit Model(const T& w) : w(w) {}
        explicit Model(T&& w) : w(std::move(w)) {}

        std::unique_ptr<Concept> clone() const override {
            return std::make_unique<Model<T>>(w);
        }

        void draw(Context& ctx, Rect r) override {
            /* The return values of the leaf draws -- a column count from a
             * label, an inner rect from a box -- are useful to a caller
             * drawing by hand and meaningless to a tree, which already knows
             * where everything goes. */
            (void)w.draw(ctx, r);
        }

        int focus_count() const override {
            if constexpr (detail::is_container<T>::value) return w.focus_count();
            else if constexpr (detail::is_focusable<T>::value) return 1;
            else return 0;
        }

        void apply_focus(int& next, int active) override {
            if constexpr (detail::is_container<T>::value) {
                w.apply_focus(next, active);
            } else if constexpr (detail::is_focusable<T>::value) {
                w.focused(next == active);
                ++next;
            }
        }

        bool dispatch(const Event& ev, Rect r, int& next, int active, int* hit) override {
            if constexpr (detail::is_container<T>::value) {
                return w.dispatch_tree(ev, r, next, active, hit);
            } else if constexpr (detail::is_focusable<T>::value) {
                const int self = next++;
                /* The focus rule, enforced here and nowhere else: a key
                 * reaches a widget only when that widget is the focused one. A
                 * mouse event is positional and is offered to every widget,
                 * which decides for itself whether the pointer was inside it;
                 * the one that takes it takes the focus with it. */
                if (ev.kind == HIKE_EVENT_MOUSE) {
                    if (!w.dispatch(r, ev)) return false;
                    if (hit) *hit = self;
                    return true;
                }
                if (self != active) return false;
                return w.dispatch(r, ev);
            } else {
                (void)ev; (void)r; (void)next; (void)active; (void)hit;
                return false;
            }
        }

        T w;
    };

    std::unique_ptr<Concept> model_;
};

/* One child of a container: how much space it asks for, and what to draw in
 * it. The rect is the one the last layout gave it, kept so an event can be
 * routed to the widget the user pointed at. */
struct Child {
    Size size{};
    Node node;
    Rect rect{};
};

/* The child wrappers. These take a node BY VALUE, so
 * hike::fixed(10, hike::button("Save")) moves the button into the tree and the
 * temporary can die immediately afterwards. */
Child fixed(int cells, Node node);
Child weight(int w, Node node);
Child content(int cells, Node node);

/* The same three without a child, for the low-level split below, where there
 * is nothing to draw and only rects are wanted. */
inline Size fixed(int cells) { return hike_fixed(cells); }
inline Size weight(int w) { return hike_weight(w); }
inline Size content(int cells) { return hike_content(cells); }

/* A content-sized child that measures itself. Only widgets that can say how
 * wide they want to be have measure(); asking any other widget for a content
 * size is a compile error rather than a silent zero, because a zero-width
 * child is exactly the kind of result nobody can explain. */
template <class T, class = std::enable_if_t<detail::has_measure<std::decay_t<T>>::value>>
Child content(T&& widget) {
    const int cells = widget.measure();
    return content(cells, Node(std::forward<T>(widget)));
}

/* --------------------------------------------------------------- layout */

/* A row or a column, owning its children.
 *
 * Configuration chains -- gap, pad, and nothing else -- and children are
 * arguments to row() and column(). The split itself is hike_layout_split, with
 * the sizes taken from the children in order, so the C rounding rules apply
 * unchanged and a tree lays out identically to the equivalent C call. */
class Layout {
public:
    Layout() = default;

    template <class... Cs>
    Layout(hike_layout base, Cs&&... children) : l_(base) {
        children_.reserve(sizeof...(Cs));
        (children_.push_back(to_child(std::forward<Cs>(children))), ...);
    }

    Layout& gap(int cells) { l_.gap = cells; return *this; }
    Layout& pad(int all) { l_ = hike_layout_pad(l_, all); return *this; }
    Layout& pad(int horizontal, int vertical);
    Layout& pad(int left, int top, int right, int bottom);

    int size() const { return int(children_.size()); }
    const hike_layout& raw() const { return l_; }
    /* The rect a child was last given. Empty before the first draw. */
    Rect child_rect(int index) const;

    void draw(Context& ctx, Rect area);
    /* Draws, having first numbered the focusable widgets in traversal order
     * and told the ring how many there are. This is the form nearly every
     * program wants: the caller stops counting widgets and the count cannot go
     * stale, because it is recomputed from the tree every frame. */
    void draw(Context& ctx, Rect area, Focus& focus);

    /* Routes an event. The ring sees it first and takes Tab and Shift+Tab;
     * what is left goes to the focused widget, or, for a mouse event, to
     * whichever widget the pointer was over. Must follow a draw, which is what
     * establishes where the widgets are. */
    bool dispatch(const Event& ev, Focus& focus);

    /* The tree interface, used by Node. Public because a container written
     * outside this header would have to implement the same three, and a
     * protocol only our own types can satisfy is not a protocol. */
    int focus_count() const;
    void apply_focus(int& next, int active);
    bool dispatch_tree(const Event& ev, Rect area, int& next, int active, int* hit);

private:
    static Child to_child(Child c) { return c; }
    template <class T, class = std::enable_if_t<!std::is_same_v<std::decay_t<T>, Child>>>
    static Child to_child(T&& widget) {
        /* A bare widget with no wrapper is an equal share. Some default is
         * needed and this is the one that stays sensible as children are added
         * and removed; a content default would make a row of labels collapse
         * to the left with unused space at the end. */
        return Child{hike_weight(1), Node(std::forward<T>(widget)), Rect{}};
    }

    hike_layout l_ = hike_row();
    std::vector<Child> children_;
    Rect area_{};
};

/* row(children...) and column(children...). A child is fixed(n, w),
 * weight(n, w), content(w), or a bare widget, which is an equal share. */
template <class... Cs>
Layout row(Cs&&... children) {
    return Layout(hike_row(), std::forward<Cs>(children)...);
}

template <class... Cs>
Layout column(Cs&&... children) {
    return Layout(hike_column(), std::forward<Cs>(children)...);
}

/* A frame with an optional title, which may contain one child drawn inside the
 * border. It is a container, so a row inside a box is a row inside a box. */
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
    Box& child(Node n) { child_ = std::move(n); return *this; }

    /* Returns the rect inside the frame, which is what a caller drawing by
     * hand needs next. */
    Rect draw(Context& ctx, Rect r);

    int focus_count() const { return child_.focus_count(); }
    void apply_focus(int& next, int active) { child_.apply_focus(next, active); }
    bool dispatch_tree(const Event& ev, Rect area, int& next, int active, int* hit);

private:
    std::string title_;
    Border border_ = HIKE_BORDER_SINGLE;
    Style style_;
    Node child_;
    Rect inner_{};
};

/* The one-liner constructors. These exist so a widget reads as a call rather
 * than a declaration: hike::button("Save").width(12) instead of a named Button
 * object that is used once. */
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

/* The bare split, for a caller that wants rects and will draw into them
 * itself. Kept because it is the faithful wrapping of hike_layout_split, and a
 * C++ layer that could not reach a public C function would no longer be a
 * wrapper of the whole surface. */
std::vector<Rect> split(hike_layout layout, Rect area, const std::vector<Size>& sizes);

/* Columns a string occupies once printed, not bytes and not code points. */
int text_width(std::string_view utf8);
inline int char_width(char32_t cp) { return hike_char_width(std::uint32_t(cp)); }

inline bool contains(Rect r, int x, int y) { return hike_rect_contains(r, x, y); }
inline Rect intersect(Rect a, Rect b) { return hike_rect_intersect(a, b); }
inline Rect inset(Rect r, int amount) { return hike_rect_inset(r, amount); }

} // namespace hike

#endif /* HIKE_HPP */
