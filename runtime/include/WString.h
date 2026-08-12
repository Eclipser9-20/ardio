// WString.h -- ardio's own fixed-capacity String.
//
// DESIGN TRADE-OFF -- FIXED CAPACITY, NO HEAP
// -------------------------------------------
// Arduino's String is heap-backed: it calls realloc() as it grows, which on a
// 2 KB ATmega328P is the single most common cause of heap fragmentation and of
// sketches that run for an hour and then die. ardio's String instead carries a
// fixed internal buffer of ARDIO_STRING_CAPACITY (32) characters plus a
// terminator, stored inline in the object.
//
// Consequences, stated plainly:
//   * A String costs 34 bytes wherever it lives -- including inside arrays and
//     on the stack. Twenty of them is a third of RAM.
//   * Appends TRUNCATE at capacity. They never fail, never allocate, and never
//     fragment; characters past the limit are silently dropped. Call length()
//     if you need to know whether everything fit.
//   * Every operation runs in bounded time with no allocator involved, so the
//     memory profile of a sketch is knowable from its source.
//
// This suits label/display sketches, which build short lines of text. It does
// not suit accumulating an unbounded input buffer; use a char array for that.
//
// NAMED METHODS INSTEAD OF OPERATORS
// ----------------------------------
// Arduino spells appending as `text += "HELLO"`. ardio's own compiler does not
// parse operator overloads at all -- a definition of `operator+=` is a syntax
// error the moment this header is included, whether or not a sketch ever uses
// it -- so every operator is replaced by an ordinary named method:
//
//      text += "HELLO";        becomes     text.concat("HELLO");
//      text += count;                      text.concatInt(count);
//      text += other;                      text.concatString(&other);
//      text = "READY";                     text.set("READY");
//      if (text == "READY")                if (text.equals("READY"))
//      String s = a + b;                   s.setString(&a); s.concatString(&b);
//
// A String that compiles beats one that does not. There is also no overloading
// of any kind here: one method per name, one constructor, no default arguments.
// Where Arduino would take a `const String &`, this takes a `const String *`,
// because references to class types are not usable either -- hence the `&other`
// in the examples above.
//
// The character work itself lives in runtime/string.S, assembled by ardio's own
// assembler. This header is a thin, allocation-free shell over those routines.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_WSTRING_H
#define ARDIO_WSTRING_H

#ifndef ARDIO_STRING_CAPACITY
#define ARDIO_STRING_CAPACITY 32
#endif

// The buffer size the routines below are handed: the characters plus the
// terminator they always leave room for.
#define ARDIO_STRING_BUFFER (ARDIO_STRING_CAPACITY + 1)

// Longest int16_t decimal is "-32768", so seven bytes with the terminator.
#define ARDIO_STRING_INT_BUFFER 7

// ---------------------------------------------------------------------------
// The primitives, implemented in runtime/string.S. `capacity` always means the
// size of the whole destination buffer, terminator included; the copy and
// append routines write at most capacity-1 characters and always terminate.
// ---------------------------------------------------------------------------

unsigned int str_len(const char *s);
void str_copy(char *dst, const char *src, unsigned int capacity);
void str_append(char *dst, const char *src, unsigned int capacity);
int str_compare(const char *a, const char *b);
void str_from_int(char *dst, int value);
int str_index_of(const char *s, char c);
unsigned char str_char_at(const char *s, unsigned int index);

class String {
public:
    String() {
        buffer_[0] = 0;
        length_ = 0;
    }

    // ---- reading -----------------------------------------------------------

    unsigned int length() const { return length_; }

    // Bytes of text this String can hold, terminator excluded.
    unsigned int capacity() const { return ARDIO_STRING_CAPACITY; }

    const char *c_str() const { return buffer_; }

    // Out of range reads as 0 rather than running off the end of the buffer.
    char charAt(unsigned int index) const {
        return (char)str_char_at(buffer_, index);
    }

    void setCharAt(unsigned int index, char c) {
        if (index < length_) buffer_[index] = c;
    }

    // ---- assigning ---------------------------------------------------------

    void clear() {
        buffer_[0] = 0;
        length_ = 0;
    }

    void set(const char *text) {
        str_copy(buffer_, text, ARDIO_STRING_BUFFER);
        length_ = str_len(buffer_);
    }

    void setString(const String *other) { set(other->buffer_); }

    void setInt(int value) {
        char digits[ARDIO_STRING_INT_BUFFER];
        str_from_int(digits, value);
        set(digits);
    }

    // ---- appending (truncates at capacity; see the note at the top) ---------

    void concat(const char *text) {
        str_append(buffer_, text, ARDIO_STRING_BUFFER);
        length_ = str_len(buffer_);
    }

    void concatString(const String *other) { concat(other->buffer_); }

    void concatChar(char c) {
        char one[2];
        one[0] = c;
        one[1] = 0;
        concat(one);
    }

    void concatInt(int value) {
        char digits[ARDIO_STRING_INT_BUFFER];
        str_from_int(digits, value);
        concat(digits);
    }

    // ---- comparing ---------------------------------------------------------

    // Negative, zero or positive, like strcmp.
    int compareTo(const char *text) const { return str_compare(buffer_, text); }

    bool equals(const char *text) const { return str_compare(buffer_, text) == 0; }

    bool equalsString(const String *other) const {
        return str_compare(buffer_, other->buffer_) == 0;
    }

    bool startsWith(const char *prefix) const {
        unsigned int i = 0;
        while (prefix[i] != 0) {
            if (i >= length_) return false;
            if (buffer_[i] != prefix[i]) return false;
            i = i + 1;
        }
        return true;
    }

    bool endsWith(const char *suffix) const {
        unsigned int n = str_len(suffix);
        if (n > length_) return false;
        unsigned int start = length_ - n;
        unsigned int i = 0;
        while (i < n) {
            if (buffer_[start + i] != suffix[i]) return false;
            i = i + 1;
        }
        return true;
    }

    // ---- searching and slicing ---------------------------------------------

    // Index of the first `c`, or -1 when it does not occur.
    int indexOf(char c) const { return str_index_of(buffer_, c); }

    // Half-open [from, to) into `out`, which is cleared first. Arduino returns
    // a fresh String; returning one by value would copy the whole 34-byte
    // object through the return slot, so the destination is passed in instead.
    void substring(unsigned int from, unsigned int to, String *out) const {
        out->buffer_[0] = 0;
        out->length_ = 0;
        if (to > length_) to = length_;
        unsigned int i = from;
        unsigned int n = 0;
        while (i < to) {
            if (n >= ARDIO_STRING_CAPACITY) break;
            out->buffer_[n] = buffer_[i];
            n = n + 1;
            i = i + 1;
        }
        out->buffer_[n] = 0;
        out->length_ = n;
    }

    // ---- whole-string edits ------------------------------------------------

    void toUpperCase() {
        unsigned int i = 0;
        while (i < length_) {
            if (buffer_[i] >= 'a' && buffer_[i] <= 'z')
                buffer_[i] = buffer_[i] - 32;
            i = i + 1;
        }
    }

    void toLowerCase() {
        unsigned int i = 0;
        while (i < length_) {
            if (buffer_[i] >= 'A' && buffer_[i] <= 'Z')
                buffer_[i] = buffer_[i] + 32;
            i = i + 1;
        }
    }

    // Decimal value of a leading integer, with an optional minus sign.
    long toInt() const {
        unsigned int i = 0;
        bool negative = false;
        if (buffer_[i] == '-') {
            negative = true;
            i = i + 1;
        }
        long value = 0;
        while (buffer_[i] >= '0' && buffer_[i] <= '9') {
            value = value * 10 + (buffer_[i] - '0');
            i = i + 1;
        }
        if (negative) return -value;
        return value;
    }

    // The buffer is public so free helper functions -- and sketches that want
    // to hand the text to a C API -- can reach it without a method call.
    char buffer_[ARDIO_STRING_BUFFER];
    unsigned int length_;
};

#endif // ARDIO_WSTRING_H
