// WString.h — ardio's own fixed-capacity String, API-compatible with the
// documented Arduino String class.
//
// DESIGN TRADE-OFF — FIXED CAPACITY, NO HEAP
// ------------------------------------------
// Arduino's String is heap-backed: it calls realloc() as it grows, which on a
// 2 KB ATmega328P is the single most common cause of heap fragmentation and of
// sketches that run for an hour and then die. ardio's String instead carries a
// fixed internal buffer of ARDIO_STRING_CAPACITY (32) characters plus a
// terminator, stored inline in the object.
//
// Consequences, stated plainly:
//   * A String costs 34 bytes wherever it lives — including inside arrays and
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
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_WSTRING_H
#define ARDIO_WSTRING_H

#ifndef ARDIO_STRING_CAPACITY
#define ARDIO_STRING_CAPACITY 32
#endif

class String {
public:
    String();
    String(const char *text);
    String(char c);
    String(int value);
    String(unsigned int value);
    String(long value);
    String(unsigned long value);
    String(const String &other);

    String &operator=(const char *text);
    String &operator=(const String &other);

    // Appending. Truncates at ARDIO_STRING_CAPACITY; see the note above.
    String &operator+=(const char *text);
    String &operator+=(const String &other);
    String &operator+=(char c);
    String &operator+=(int value);
    String &operator+=(long value);

    // concat() is the named form of the same operation, returning whether the
    // whole argument fit.
    bool concat(const char *text);
    bool concat(const String &other);
    bool concat(char c);
    bool concat(int value);
    bool concat(long value);

    unsigned int length() const;
    char charAt(unsigned int index) const;
    void setCharAt(unsigned int index, char c);
    const char *c_str() const;

    // Half-open [from, to). substring(from) is spelled substring(from, length()).
    String substring(unsigned int from, unsigned int to) const;

    int indexOf(char c) const;
    int indexOf(const char *text) const;

    bool equals(const String &other) const;
    bool equals(const char *text) const;
    bool startsWith(const char *prefix) const;
    bool endsWith(const char *suffix) const;

    void toUpperCase();
    void toLowerCase();
    void trim();
    void remove(unsigned int index, unsigned int count);

    long toInt() const;

private:
    char buffer_[ARDIO_STRING_CAPACITY + 1];
    unsigned int length_;
};

// Concatenation. Only the forms a sketch actually writes are provided; each
// yields a fresh String subject to the same fixed capacity.
String operator+(const String &lhs, const String &rhs);
String operator+(const String &lhs, const char *rhs);
String operator+(const char *lhs, const String &rhs);
String operator+(const String &lhs, char rhs);
String operator+(const String &lhs, int rhs);
String operator+(const String &lhs, long rhs);

bool operator==(const String &lhs, const String &rhs);
bool operator==(const String &lhs, const char *rhs);
bool operator==(const char *lhs, const String &rhs);
bool operator!=(const String &lhs, const String &rhs);
bool operator!=(const String &lhs, const char *rhs);

#endif // ARDIO_WSTRING_H
