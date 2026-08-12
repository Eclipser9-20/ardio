// WString.h -- ardio's own String: a fixed-capacity view over storage the
// sketch supplies.
//
// HOW TO DECLARE ONE
// ------------------
//      char line_storage[33];
//      String line(line_storage, 33);      // 32 characters plus a terminator
//
//      line.set("READY");
//      line.concatInt(count);
//      lcd.print(line.c_str());
//
// DESIGN TRADE-OFF -- NO HEAP, AND NO HIDDEN BUFFER EITHER
// -------------------------------------------------------
// Arduino's String is heap-backed: it calls realloc() as it grows, which on a
// 2 KB ATmega328P is the single most common cause of heap fragmentation and of
// sketches that run for an hour and then die. ardio's String never allocates.
//
// It does not carry an inline buffer either, which is the visible divergence
// from the published API. ardio's compiler can store into an array member but
// cannot read one back out from inside a method -- so a String holding its own
// `char buffer_[33]` would compile only as far as its first accessor. Holding
// a `char *` instead works everywhere, costs 4 bytes per String instead of 34,
// and lets every operation below be a direct call into runtime/string.S rather
// than a hand-rolled loop.
//
// Consequences, stated plainly:
//   * The storage must outlive the String. A global array or one declared in
//     the same scope is fine; a local array handed to a String that outlives
//     the function is not.
//   * Two Strings pointed at the same array alias each other.
//   * Appends TRUNCATE at capacity. They never fail, never allocate, and never
//     fragment; characters past the limit are silently dropped. Call length()
//     if you need to know whether everything fit.
//   * Every operation runs in bounded time with no allocator involved, so the
//     memory profile of a sketch is knowable from its source.
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
// Where Arduino would take a `const String &`, this takes a `const String *`,
// because references to class types are not usable either -- hence the `&other`
// in the examples above.
//
// The character work itself lives in runtime/string.S, assembled by ardio's own
// assembler. This header is a thin, allocation-free shell over those routines:
// ardio has no linker, so every method here carries an inline body that calls
// one of them, rather than a declaration that would fail at assembly time as
// "nothing implements 'String__set'".
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_WSTRING_H
#define ARDIO_WSTRING_H

// Longest int16_t decimal is "-32768", so seven bytes with the terminator.
#define ARDIO_STRING_INT_BUFFER 7

// ---------------------------------------------------------------------------
// The primitives, implemented in runtime/string.S. `capacity` always means the
// size of the whole destination buffer, terminator included; the copy and
// append routines write at most capacity-1 characters and always terminate.
//
// These are usable directly on any char array, with or without a String.
// ---------------------------------------------------------------------------

unsigned int str_len(const char *s);
void str_copy(char *dst, const char *src, unsigned int capacity);
void str_append(char *dst, const char *src, unsigned int capacity);
int str_compare(const char *a, const char *b);
void str_from_int(char *dst, int value);
int str_index_of(const char *s, char c);
int str_char_at(const char *s, unsigned int index);

class String {
public:
    // `storage` is the array to work in and `buffer_size` its whole size, the
    // terminator included -- so a String that holds 32 characters is given 33.
    // The storage is emptied, so a fresh String reads as "".
    String(char *storage, unsigned int buffer_size) {
        buffer_ = storage;
        buffer_size_ = buffer_size;
        if (buffer_size > 0) buffer_[0] = 0;
    }

    // ---- reading -----------------------------------------------------------

    unsigned int length() const { return str_len(buffer_); }

    // Bytes of text this String can hold, terminator excluded.
    unsigned int capacity() const {
        if (buffer_size_ == 0) return 0;
        return buffer_size_ - 1;
    }

    const char *c_str() const { return buffer_; }

    // Out of range reads as 0 rather than running off the end of the buffer.
    char charAt(unsigned int index) const {
        return (char)str_char_at(buffer_, index);
    }

    void setCharAt(unsigned int index, char c) {
        if (index < length()) buffer_[index] = c;
    }

    // ---- assigning ---------------------------------------------------------

    void clear() {
        if (buffer_size_ > 0) buffer_[0] = 0;
    }

    void set(const char *text) { str_copy(buffer_, text, buffer_size_); }

    void setString(const String *other) { set(other->c_str()); }

    void setInt(int value) {
        char digits[ARDIO_STRING_INT_BUFFER];
        str_from_int(digits, value);
        set(digits);
    }

    // ---- appending (truncates at capacity; see the note at the top) ---------

    void concat(const char *text) { str_append(buffer_, text, buffer_size_); }

    void concatString(const String *other) { concat(other->c_str()); }

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
        return str_compare(buffer_, other->c_str()) == 0;
    }

    bool startsWith(const char *prefix) const {
        unsigned int i = 0;
        unsigned int n = length();
        while (prefix[i] != 0) {
            if (i >= n) return false;
            if (buffer_[i] != prefix[i]) return false;
            i = i + 1;
        }
        return true;
    }

    bool endsWith(const char *suffix) const {
        unsigned int n = str_len(suffix);
        unsigned int have = length();
        if (n > have) return false;
        unsigned int start = have - n;
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
    // a fresh String; there is nowhere to allocate one, so the destination --
    // with its own storage -- is passed in instead.
    void substring(unsigned int from, unsigned int to, String *out) const {
        out->clear();
        unsigned int n = length();
        if (to > n) to = n;
        unsigned int i = from;
        while (i < to) {
            out->concatChar((char)str_char_at(buffer_, i));
            i = i + 1;
        }
    }

    // ---- whole-string edits ------------------------------------------------

    void toUpperCase() {
        unsigned int i = 0;
        unsigned int n = length();
        while (i < n) {
            if (buffer_[i] >= 'a' && buffer_[i] <= 'z')
                buffer_[i] = buffer_[i] - 32;
            i = i + 1;
        }
    }

    void toLowerCase() {
        unsigned int i = 0;
        unsigned int n = length();
        while (i < n) {
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
            value = value * 10 + (long)(buffer_[i] - '0');
            i = i + 1;
        }
        if (negative) return -value;
        return value;
    }

    // The storage this String works in, so a sketch can hand the text to a C
    // API without a method call.
    char *buffer_;

    // Its whole size, terminator included.
    unsigned int buffer_size_;
};

#endif // ARDIO_WSTRING_H
