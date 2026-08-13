/* libhike -- the C++ layer's implementation.
 *
 * Every function here ends in a call to a public C entry point. Nothing in
 * this file includes a private header, reads a struct the C API keeps opaque,
 * or reimplements a rule the C layer already decides -- the scrolling of an
 * input, the rounding of a layout and the wrapping of the focus ring all
 * happen once, in C, and are observed from here.
 *
 * The one recurring piece of work that is genuinely C++'s own is lifetime. C
 * widgets take arrays of const char* that the caller keeps alive; the C++
 * builders hold std::string, so each draw call builds a vector of pointers
 * into those strings that lives exactly as long as the C call does. It is a
 * small cost per frame and it is the price of the wrapper owning its text,
 * which is the thing that makes it pleasant to use.
 */

#include "hike/hike.hpp"

#include <cstring>
#include <utility>

namespace hike {
namespace {

/* The C API takes NUL-terminated UTF-8 and a string_view is not required to
 * be. Copying is the only correct answer: pointing at the view's data and
 * hoping for a terminator would read past the end for any substring. */
std::string terminated(std::string_view sv) { return std::string(sv); }

/* Builds the array of pointers a C widget wants from a vector of strings. The
 * storage vector must outlive the call that uses the result. */
const char* const* pointers(const std::vector<std::string>& strings,
                            std::vector<const char*>& storage) {
    storage.clear();
    storage.reserve(strings.size());
    for (const auto& s : strings) storage.push_back(s.c_str());
    return storage.empty() ? nullptr : storage.data();
}

} // namespace

/* ----------------------------------------------------------------- error */

Error::Error(Status status)
    : std::runtime_error(hike_status_text(status)), status_(status) {}

/* --------------------------------------------------------------- context */

Context::Context(const Options& options) : ctx_(nullptr) {
    hike_options raw = options.raw();
    hike_status status = hike_init(&ctx_, &raw);
    if (status != HIKE_OK) {
        ctx_ = nullptr;
        throw Error(status);
    }
}

Context Context::adopt(hike_context* raw) {
    if (!raw) throw Error(HIKE_ERR_INVALID_ARGUMENT);
    return Context(raw);
}

Context::~Context() {
    if (ctx_) hike_shutdown(ctx_);
}

Context::Context(Context&& other) noexcept : ctx_(other.ctx_) {
    other.ctx_ = nullptr;
}

Context& Context::operator=(Context&& other) noexcept {
    if (this != &other) {
        if (ctx_) hike_shutdown(ctx_);
        ctx_ = other.ctx_;
        other.ctx_ = nullptr;
    }
    return *this;
}

int Context::width() const { return hike_width(ctx_); }
int Context::height() const { return hike_height(ctx_); }
Rect Context::bounds() const { return Rect{0, 0, width(), height()}; }
Depth Context::depth() const { return hike_depth(ctx_); }

void Context::clear() { hike_clear(ctx_); }
void Context::set_cell(int x, int y, Cell cell) { hike_set_cell(ctx_, x, y, cell); }
Cell Context::cell(int x, int y) const { return hike_get_cell(ctx_, x, y); }

int Context::text(int x, int y, std::string_view utf8, Style style) {
    const std::string s = terminated(utf8);
    hike_style st = style;
    return hike_text(ctx_, x, y, s.c_str(), st.fg, st.bg, st.attrs);
}

void Context::fill(Rect r, Cell cell) { hike_fill(ctx_, r, cell); }
void Context::present() { hike_present(ctx_); }
void Context::invalidate() { hike_invalidate(ctx_); }
void Context::cursor(int x, int y, bool visible) { hike_set_cursor(ctx_, x, y, visible); }
void Context::push_clip(Rect r) { hike_push_clip(ctx_, r); }
void Context::pop_clip() { hike_pop_clip(ctx_); }

std::optional<Event> Context::poll(int timeout_ms) {
    Event ev{};
    if (!hike_poll(ctx_, &ev, timeout_ms)) return std::nullopt;
    return ev;
}

Clip::Clip(Context& ctx, Rect r) : ctx_(ctx) { ctx_.push_clip(r); }
Clip::~Clip() { ctx_.pop_clip(); }

/* ---------------------------------------------------------------- layout */

Layout& Layout::pad(int horizontal, int vertical) {
    l_.pad_left = l_.pad_right = horizontal;
    l_.pad_top = l_.pad_bottom = vertical;
    return *this;
}

Layout& Layout::pad(int left, int top, int right, int bottom) {
    l_.pad_left = left;
    l_.pad_top = top;
    l_.pad_right = right;
    l_.pad_bottom = bottom;
    return *this;
}

std::vector<Rect> Layout::split(Rect area) const { return split(area, sizes_); }

std::vector<Rect> Layout::split(Rect area, const std::vector<Size>& sizes) const {
    std::vector<Rect> out(sizes.size());
    if (sizes.empty()) return out;
    hike_layout_split(l_, area, sizes.data(), int(sizes.size()), out.data());
    return out;
}

/* --------------------------------------------------------------- widgets */

int Label::draw(Context& ctx, Rect r) const {
    return hike_label(ctx.raw(), r, text_.c_str(), align_, style_);
}

Rect Box::draw(Context& ctx, Rect r) const {
    return hike_box(ctx.raw(), r, border_,
                    title_.empty() ? nullptr : title_.c_str(), style_);
}

hike_button Button::make() const {
    hike_button b = hike_button_make(label_.c_str());
    b.align = align_;
    b.focused = focused_;
    b.disabled = disabled_;
    b.theme = theme_;
    return b;
}

int Button::measure() const {
    return width_ >= 0 ? width_ : hike_text_width(label_.c_str()) + 4;
}

void Button::draw(Context& ctx, Rect r) const {
    hike_button b = make();
    if (width_ >= 0 && width_ < r.w) r.w = width_;
    hike_button_draw(ctx.raw(), r, &b);
}

bool Button::dispatch(Rect r, const Event& ev) const {
    hike_button b = make();
    if (width_ >= 0 && width_ < r.w) r.w = width_;
    if (!hike_button_event(&b, r, &ev)) return false;
    if (on_click_) on_click_();
    return true;
}

hike_checkbox Checkbox::make() const {
    hike_checkbox c = hike_checkbox_make(label_.c_str(), checked_);
    c.focused = focused_;
    c.disabled = disabled_;
    c.theme = theme_;
    return c;
}

void Checkbox::draw(Context& ctx, Rect r) const {
    hike_checkbox c = make();
    hike_checkbox_draw(ctx.raw(), r, &c);
}

bool Checkbox::dispatch(Rect r, const Event& ev) {
    hike_checkbox c = make();
    if (!hike_checkbox_event(&c, r, &ev)) return false;
    checked_ = c.checked;
    if (on_change_) on_change_(checked_);
    return true;
}

hike_radio_group RadioGroup::make(std::vector<const char*>& storage) const {
    hike_radio_group g = hike_radio_make(pointers(labels_, storage),
                                         int(labels_.size()), selected_);
    g.focused = focused_;
    g.disabled = disabled_;
    g.theme = theme_;
    return g;
}

void RadioGroup::draw(Context& ctx, Rect r) const {
    std::vector<const char*> storage;
    hike_radio_group g = make(storage);
    hike_radio_draw(ctx.raw(), r, &g);
}

bool RadioGroup::dispatch(Rect r, const Event& ev) {
    std::vector<const char*> storage;
    hike_radio_group g = make(storage);
    if (!hike_radio_event(&g, r, &ev)) return false;
    selected_ = g.selected;
    if (on_change_) on_change_(selected_);
    return true;
}

hike_list List::make(std::vector<const char*>& storage) const {
    hike_list l = hike_list_make(pointers(items_, storage), int(items_.size()));
    l.selected = selected_;
    l.scroll = scroll_;
    l.focused = focused_;
    l.theme = theme_;
    return l;
}

void List::take(const hike_list& l) {
    selected_ = l.selected;
    scroll_ = l.scroll;
}

void List::draw(Context& ctx, Rect r) {
    std::vector<const char*> storage;
    hike_list l = make(storage);
    /* Drawing is what scrolls the selection into view in the C layer, so the
     * scroll position has to come back out afterwards or the C++ object would
     * report a stale one. */
    hike_list_draw(ctx.raw(), r, &l);
    take(l);
}

bool List::dispatch(int height, const Event& ev) {
    std::vector<const char*> storage;
    hike_list l = make(storage);
    if (!hike_list_event(&l, height, &ev)) {
        take(l);
        return false;
    }
    take(l);
    if (on_select_) on_select_(selected_);
    return true;
}

Input::Input(std::size_t capacity) : buf_(capacity ? capacity : 1, '\0') {
    in_ = hike_input_make(buf_.data(), buf_.size());
}

Input& Input::text(std::string_view value) {
    const std::string s = terminated(value);
    hike_input_set_text(&in_, s.c_str());
    return *this;
}

std::string_view Input::value() const {
    return std::string_view(in_.buf, in_.len);
}

void Input::sync() {
    in_.buf = buf_.data();
    in_.cap = buf_.size();
    in_.masked = masked_;
    in_.focused = focused_;
    in_.theme = theme_;
    in_.placeholder = placeholder_.empty() ? nullptr : placeholder_.c_str();
}

void Input::draw(Context& ctx, Rect r) {
    sync();
    hike_input_draw(ctx.raw(), r, &in_);
}

bool Input::dispatch(int width, const Event& ev) {
    sync();
    const std::size_t before = in_.len;
    if (!hike_input_event(&in_, width, &ev)) return false;
    if (on_change_ && in_.len != before) on_change_(value());
    return true;
}

void Progress::draw(Context& ctx, Rect r) const {
    hike_progress p = hike_progress_make(value_);
    p.show_percent = percent_;
    p.theme = theme_;
    hike_progress_draw(ctx.raw(), r, &p);
}

hike_textview TextView::make(std::vector<const char*>& storage) const {
    hike_textview v = hike_textview_make(pointers(lines_, storage), int(lines_.size()));
    v.scroll = scroll_;
    v.hscroll = hscroll_;
    v.focused = focused_;
    v.theme = theme_;
    return v;
}

void TextView::draw(Context& ctx, Rect r) {
    std::vector<const char*> storage;
    hike_textview v = make(storage);
    hike_textview_draw(ctx.raw(), r, &v);
    scroll_ = v.scroll;
    hscroll_ = v.hscroll;
}

bool TextView::dispatch(int height, const Event& ev) {
    std::vector<const char*> storage;
    hike_textview v = make(storage);
    const bool moved = hike_textview_event(&v, height, &ev);
    scroll_ = v.scroll;
    hscroll_ = v.hscroll;
    return moved;
}

hike_tabs Tabs::make(std::vector<const char*>& storage) const {
    hike_tabs t = hike_tabs_make(pointers(labels_, storage), int(labels_.size()));
    t.selected = selected_;
    t.focused = focused_;
    t.theme = theme_;
    return t;
}

void Tabs::draw(Context& ctx, Rect r) const {
    std::vector<const char*> storage;
    hike_tabs t = make(storage);
    hike_tabs_draw(ctx.raw(), r, &t);
}

bool Tabs::dispatch(Rect r, const Event& ev) {
    std::vector<const char*> storage;
    hike_tabs t = make(storage);
    if (!hike_tabs_event(&t, r, &ev)) return false;
    selected_ = t.selected;
    if (on_change_) on_change_(selected_);
    return true;
}

/* --------------------------------------------------------------- utility */

int text_width(std::string_view utf8) {
    const std::string s = terminated(utf8);
    return hike_text_width(s.c_str());
}

} // namespace hike
