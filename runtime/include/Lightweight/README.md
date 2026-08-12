# Lightweight

A two-header container library for sketches:

```cpp
#include <Lightweight/String>
#include <Lightweight/Vector>
```

Both headers are extensionless, so the include spelling reads the way the
standard library's does. ardio's preprocessor resolves an include with a
directory component and no extension by plain path joining against each
include directory, so `<Lightweight/String>` needs nothing special — put
`runtime/include` on the include path and it is found.

## What these are

Fixed-capacity, heap-free replacements for `std::string` and `std::vector`,
with a familiar interface:

| Type           | Element        | Capacity   | SRAM per instance |
| -------------- | -------------- | ---------- | ----------------- |
| `LwString`     | `char`         | 32 chars   | 33 bytes + 2      |
| `LwVectorInt`  | `int`          | 16 items   | 32 bytes + 4      |
| `LwVectorByte` | `unsigned char`| 16 items   | 16 bytes + 4      |

The trailing "+2" / "+4" is the object itself: a pointer to the storage, and
for the vectors a size. The capacities are the compile-time constants
`LW_STRING_CAPACITY` (32), `LW_STRING_BUFFER` (33, the terminator included)
and `LW_VECTOR_CAPACITY` (16). They are the same for every instance; there is
no per-instance capacity, because without templates there is no way to make
one a type parameter.

Declare one with the macro that comes with it — that is the only declaration
that guarantees the storage is the size every method assumes:

```cpp
LW_STRING(line);            // char line_lw_storage[33]; LwString line(...);
LW_VECTOR_INT(samples);
LW_VECTOR_BYTE(packet);
```

## Why not the standard containers

Three reasons, all of them about the target rather than about taste.

**No heap.** `std::string` and `std::vector` grow by reallocating. An
ATmega328P has 2 KB of SRAM total; an allocator on that part is the single
most common cause of a sketch that runs for an hour and then dies of
fragmentation. Nothing here allocates, ever. The memory a sketch uses is
readable from its source.

**No exceptions.** `std::vector::push_back` reports a failed growth by
throwing. There is no unwinder here and no way to add one, so every operation
that can refuse says so with a return value instead.

**No templates.** ardio's compiler does not have them at all — `vector<T>`
cannot be written, not even badly. A template-free library has to ship a
separate concrete class for every element type it supports, each one a copy of
the same code with the element type changed. That is why there are exactly two
vectors, `LwVectorInt` and `LwVectorByte`, and why adding a third means
copy-pasting one of them. It is the honest cost of no templates, and it is the
reason the set is deliberately small rather than exhaustive.

## Overflow: what actually happens

Nothing here ever writes past the end of its storage. Every operation is
bounds-checked at the boundary, and the behaviour at the boundary is defined:

**`LwString` truncates.** `set`, `append`, `appendInt`, `appendChar` and
`substring` write at most 32 characters plus a terminator. Characters that do
not fit are silently dropped; the string stays terminated and stays valid.
Nothing fails, nothing throws, nothing allocates. Call `length()` if you need
to know whether everything fit, or `full()` to ask before appending.

* `at(i)` past the end returns `0`.
* `setAt(i, c)` past the end does nothing. It also refuses `c == 0`, which
  would otherwise shorten the string behind its back.
* `substring(from, to, out)` clamps `to` to the length, yields an empty result
  when `from >= to`, and stops early if the destination fills up.

**`LwVectorInt` / `LwVectorByte` reject.** A full vector does not grow and
does not overwrite:

* `push_back(v)` on a full vector does nothing and returns `false`. The
  existing elements and the size are untouched.
* `pop_back()` on an empty vector does nothing and returns `false`.
* `at(i)` with `i >= size()` returns `0`.
* `set(i, v)` with `i >= size()` does nothing and returns `false`.
* `front()` / `back()` on an empty vector return `0`.

So a sketch that checks the return value can react, and a sketch that ignores
it drops the sample instead of corrupting memory.

## Things the compiler could not express

These shape the interface, and they are worth knowing before reading the
headers and wondering why they look like this.

**The storage is passed in.** ardio's code generator supports 8- and 16-bit
class fields only. A class can store *into* an array member but cannot read one
back out from inside a method — the compiler says `field 'data_' of class 'X':
only 8- and 16-bit fields are supported`. So a container cannot carry its own
inline buffer; it holds a pointer, and the `LW_*` macros declare the array
alongside it. Consequences: the storage must outlive the container, two
containers pointed at the same array alias each other, and handing the
constructor a shorter array of your own is the one way to defeat the bounds
checking.

**No overloads, no operators.** Two methods or two constructors sharing a name
emit the same assembly label and collide at assembly time, and operator
overloads are not parsed at all. So there is exactly one constructor per class
and every operation has its own name: `set` / `setInt`, `append` /
`appendInt` / `appendChar` / `appendString`, `at` / `set`. Where the standard
containers would take a reference, these take a pointer — `a.appendString(&b)`
— because references to class types are not usable either. `substring` writes
into a destination passed in rather than returning a new string, because there
is nowhere to allocate one.

**Names are prefixed.** The classes are `LwString`, `LwVectorInt` and
`LwVectorByte`, not `String` and `Vector`. Namespaces are flattened by the
parser, so `Lightweight::String` would simply become `String` and collide with
the `String` in `<WString.h>`. The prefix is what lets a sketch include both.

**Locals are tight.** A function's frame is limited — at the time of writing it
reaches 62 bytes — so a single local `LW_STRING` (33 bytes) is fine and two are
not; the compiler reports `function 'f' needs N bytes of locals; the frame
pointer reaches only 62`. Declare containers at file scope when a function needs
more than one.

## Runtime

`LwString` calls the routines in `runtime/string.S` — `str_len`, `str_copy`,
`str_append`, `str_compare`, `str_from_int`, `str_index_of`, `str_char_at` —
rather than reimplementing them, so the truncation rules are exactly the ones
that file already guarantees. The vectors need no runtime support at all.

Part of ardio. Licensed under the GNU General Public License v3.
