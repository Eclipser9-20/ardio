# ardio Phase 1 Implementation Plan

> Tasks are ordered and independently testable. Each ends with a passing test suite
> and a commit. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `ardio` into a working uploader — it finds a connected ATmega328P Nano, compiles a sketch, flashes it over a from-scratch STK500v1 implementation, and opens a serial monitor.

**Architecture:** A static library `libardio` holds all logic, split by responsibility: `platform/` (IOKit enumeration, termios serial), `protocol/` (STK500v1 as pure byte-encoders plus a driver), `board/` (the shared board description consumed by both the uploader and, later, the emulator), `toolchain/` (search-path resolution + config), and `build/` (compiler command construction). The `ardio` CLI is a thin dispatcher over it. The design rule throughout: anything that can be a pure function from inputs to bytes *is* one, so it can be tested without hardware.

**Tech Stack:** C++20, CMake 4.4.2, Apple Clang, macOS IOKit + termios. Zero third-party dependencies — the test harness and TOML subset parser are written in-tree.

## Global Constraints

- **Public standalone project.** All original work; no third-party or privately-held code.
- **No third-party libraries.** No `arduino-cli`, `avrdude`, `esptool`, `bossac`, no vendored package manager, no external test framework.
- **C++20**, compiled with `-Wall -Wextra -Werror`.
- **macOS first.** All OS-specific code lives under `platform/macos/` behind interfaces declared in `platform/`. No `#ifdef __APPLE__` outside that directory.
- **No silent downloads.** Nothing is fetched from the network without an explicit user prompt.
- **Every public header** goes in `include/ardio/`; implementation in `src/`.
- Target board for this phase: **ATmega328P Nano**, USB `0x1A86:0x7523` (CH340), signature `0x1E 0x95 0x0F`, flash page size 128 bytes, bootloader baud 115200 with 57600 fallback.

---

### Task 1: Project skeleton, build system, test harness

**Files:**
- Create: `CMakeLists.txt`
- Create: `tests/harness.h`
- Create: `tests/test_harness.cpp`
- Create: `.gitignore`

**Interfaces:**
- Consumes: nothing.
- Produces: `TEST(name) { ... }` test-registration macro; `CHECK(expr)` and `CHECK_EQ(a, b)` assertions; `ardio_tests` CMake target; `libardio` static library target that later tasks add sources to.

- [ ] **Step 1: Write the failing test**

Create `tests/test_harness.cpp`:

```cpp
#include "harness.h"

TEST(harness_reports_equality) {
    CHECK_EQ(2 + 2, 4);
    CHECK(true);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd ~/ardio && cmake -S . -B build && cmake --build build`
Expected: FAIL — `CMakeLists.txt` does not exist, then `harness.h` not found.

- [ ] **Step 3: Write minimal implementation**

Create `tests/harness.h`:

```cpp
#pragma once
#include <cstdio>
#include <string>
#include <vector>
#include <functional>

namespace ardio_test {

struct Case { const char* name; std::function<void()> fn; };

inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
inline const char*& current() { static const char* c = ""; return c; }

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const std::string& msg) {
    std::printf("  FAIL %s\n    %s:%d: %s\n", current(), file, line, msg.c_str());
    ++failures();
}

inline int run_all() {
    for (auto& c : registry()) {
        current() = c.name;
        int before = failures();
        c.fn();
        if (failures() == before) std::printf("  ok   %s\n", c.name);
    }
    std::printf("\n%zu tests, %d failures\n", registry().size(), failures());
    return failures() == 0 ? 0 : 1;
}

} // namespace ardio_test

#define TEST(name)                                                            \
    static void test_##name();                                                \
    static ::ardio_test::Register reg_##name(#name, test_##name);             \
    static void test_##name()

#define CHECK(expr)                                                           \
    do { if (!(expr)) ::ardio_test::fail(__FILE__, __LINE__, "CHECK(" #expr ")"); } while (0)

#define CHECK_EQ(a, b)                                                        \
    do { auto _a = (a); auto _b = (b);                                        \
         if (!(_a == _b))                                                     \
             ::ardio_test::fail(__FILE__, __LINE__,                           \
                 "CHECK_EQ(" #a ", " #b ") -- left=" + std::to_string(_a) +   \
                 " right=" + std::to_string(_b));                             \
    } while (0)
```

Create `tests/main.cpp`:

```cpp
#include "harness.h"
int main() { return ardio_test::run_all(); }
```

Create `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(ardio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

add_library(libardio STATIC)
set_target_properties(libardio PROPERTIES OUTPUT_NAME ardio)
target_include_directories(libardio PUBLIC include)
target_sources(libardio PRIVATE src/placeholder.cpp)

add_executable(ardio_tests tests/main.cpp tests/test_harness.cpp)
target_link_libraries(ardio_tests PRIVATE libardio)
target_include_directories(ardio_tests PRIVATE tests)

enable_testing()
add_test(NAME unit COMMAND ardio_tests)
```

Create `src/placeholder.cpp` (replaced in Task 2):

```cpp
namespace ardio { int library_linked() { return 1; } }
```

Create `.gitignore`:

```
build/
.DS_Store
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd ~/ardio && cmake -S . -B build && cmake --build build && ./build/ardio_tests`
Expected: PASS — `ok   harness_reports_equality`, `1 tests, 0 failures`.

- [ ] **Step 5: Commit**

```bash
cd ~/ardio
git add CMakeLists.txt tests .gitignore src/placeholder.cpp
git commit -m "build: add CMake skeleton and in-tree test harness"
```

---

### Task 2: Intel HEX parser

**Files:**
- Create: `include/ardio/hex.h`
- Create: `src/hex.cpp`
- Create: `tests/test_hex.cpp`
- Modify: `CMakeLists.txt`
- Delete: `src/placeholder.cpp`

**Interfaces:**
- Consumes: test harness from Task 1.
- Produces:
  - `struct ardio::HexImage { std::vector<uint8_t> data; uint32_t base_address; };`
  - `std::optional<HexImage> ardio::parse_intel_hex(std::string_view text, std::string& error);`

`.hex` is the format `avr-gcc` emits and the format the programmer uploads, so this is the seam between build and flash. Records are `:LLAAAATT<data>CC` — length, address, type, checksum. Type `00` is data, `01` is end-of-file, and types `02`/`04` are segment/linear address extensions that a 32KB AVR image never needs; treat them as an error rather than silently ignoring them.

- [ ] **Step 1: Write the failing test**

Create `tests/test_hex.cpp`:

```cpp
#include "harness.h"
#include "ardio/hex.h"
#include <string>

TEST(hex_parses_single_data_record) {
    std::string err;
    auto img = ardio::parse_intel_hex(":03000000C0FFEE50\n:00000001FF\n", err);
    CHECK(img.has_value());
    CHECK_EQ(img->data.size(), size_t(3));
    CHECK_EQ(int(img->data[0]), 0xC0);
    CHECK_EQ(int(img->data[1]), 0xFF);
    CHECK_EQ(int(img->data[2]), 0xEE);
}

TEST(hex_fills_gaps_between_records_with_0xFF) {
    std::string err;
    // one byte at 0x0000, one byte at 0x0004 -- the gap is unprogrammed flash
    auto img = ardio::parse_intel_hex(":01000000AA55\n:0100040055A6\n:00000001FF\n", err);
    CHECK(img.has_value());
    CHECK_EQ(img->data.size(), size_t(5));
    CHECK_EQ(int(img->data[1]), 0xFF);
    CHECK_EQ(int(img->data[3]), 0xFF);
    CHECK_EQ(int(img->data[4]), 0x55);
}

TEST(hex_rejects_bad_checksum) {
    std::string err;
    auto img = ardio::parse_intel_hex(":03000000C0FFEE00\n:00000001FF\n", err);
    CHECK(!img.has_value());
    CHECK(err.find("checksum") != std::string::npos);
}

TEST(hex_rejects_missing_eof_record) {
    std::string err;
    auto img = ardio::parse_intel_hex(":03000000C0FFEE50\n", err);
    CHECK(!img.has_value());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd ~/ardio && cmake --build build 2>&1 | head -5`
Expected: FAIL — `'ardio/hex.h' file not found`.

- [ ] **Step 3: Write minimal implementation**

Create `include/ardio/hex.h`:

```cpp
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ardio {

struct HexImage {
    std::vector<uint8_t> data;
    uint32_t base_address = 0;
};

// Parses Intel HEX. Gaps between records are filled with 0xFF (erased flash).
// Returns nullopt and sets `error` on malformed input.
std::optional<HexImage> parse_intel_hex(std::string_view text, std::string& error);

} // namespace ardio
```

Create `src/hex.cpp`:

```cpp
#include "ardio/hex.h"
#include <cctype>

namespace ardio {
namespace {

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Reads two hex chars at `pos`, advancing it. Returns -1 on malformed input.
int hex_byte(std::string_view s, size_t& pos) {
    if (pos + 1 >= s.size()) return -1;
    int hi = hex_nibble(s[pos]), lo = hex_nibble(s[pos + 1]);
    if (hi < 0 || lo < 0) return -1;
    pos += 2;
    return (hi << 4) | lo;
}

} // namespace

std::optional<HexImage> parse_intel_hex(std::string_view text, std::string& error) {
    HexImage img;
    bool saw_eof = false;
    size_t line_no = 0;

    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find('\n', i);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(i, end - i);
        i = end + 1;
        ++line_no;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty()) continue;

        if (line[0] != ':') {
            error = "line " + std::to_string(line_no) + ": record does not start with ':'";
            return std::nullopt;
        }

        size_t p = 1;
        int len = hex_byte(line, p), addr_hi = hex_byte(line, p),
            addr_lo = hex_byte(line, p), type = hex_byte(line, p);
        if (len < 0 || addr_hi < 0 || addr_lo < 0 || type < 0) {
            error = "line " + std::to_string(line_no) + ": truncated record header";
            return std::nullopt;
        }

        uint32_t addr = uint32_t(addr_hi) << 8 | uint32_t(addr_lo);
        int sum = len + addr_hi + addr_lo + type;

        std::vector<uint8_t> payload;
        payload.reserve(size_t(len));
        for (int n = 0; n < len; ++n) {
            int b = hex_byte(line, p);
            if (b < 0) {
                error = "line " + std::to_string(line_no) + ": truncated data";
                return std::nullopt;
            }
            payload.push_back(uint8_t(b));
            sum += b;
        }

        int checksum = hex_byte(line, p);
        if (checksum < 0) {
            error = "line " + std::to_string(line_no) + ": missing checksum";
            return std::nullopt;
        }
        if (((sum + checksum) & 0xFF) != 0) {
            error = "line " + std::to_string(line_no) + ": checksum mismatch";
            return std::nullopt;
        }

        if (type == 0x01) { saw_eof = true; break; }
        if (type != 0x00) {
            error = "line " + std::to_string(line_no) + ": unsupported record type " +
                    std::to_string(type) + " (ardio targets flat <64KB AVR images)";
            return std::nullopt;
        }

        if (addr + payload.size() > img.data.size())
            img.data.resize(addr + payload.size(), 0xFF);
        for (size_t n = 0; n < payload.size(); ++n) img.data[addr + n] = payload[n];
    }

    if (!saw_eof) {
        error = "missing end-of-file record (:00000001FF)";
        return std::nullopt;
    }
    return img;
}

} // namespace ardio
```

Modify `CMakeLists.txt` — replace the `target_sources` line and extend the test target:

```cmake
target_sources(libardio PRIVATE src/hex.cpp)

add_executable(ardio_tests tests/main.cpp tests/test_harness.cpp tests/test_hex.cpp)
```

Delete `src/placeholder.cpp`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd ~/ardio && cmake --build build && ./build/ardio_tests`
Expected: PASS — 5 tests, 0 failures.

- [ ] **Step 5: Commit**

```bash
cd ~/ardio
git add include/ardio/hex.h src/hex.cpp tests/test_hex.cpp CMakeLists.txt
git rm --cached src/placeholder.cpp 2>/dev/null; rm -f src/placeholder.cpp
git commit -m "feat: add Intel HEX parser"
```

---

### Task 3: Board description model

**Files:**
- Create: `include/ardio/board.h`
- Create: `src/board.cpp`
- Create: `tests/test_board.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class ardio::Protocol { Stk500v1 };`
  - `struct ardio::UsbId { uint16_t vid; uint16_t pid; };`
  - `struct ardio::Board { std::string id, name, mcu; Protocol protocol; std::vector<UsbId> usb_ids; std::vector<int> baud_rates; uint32_t flash_size; uint32_t page_size; std::array<uint8_t,3> signature; std::string gcc_mcu; int f_cpu; };`
  - `const std::vector<Board>& ardio::board_database();`
  - `const Board* ardio::find_board_by_id(std::string_view id);`
  - `std::vector<const Board*> ardio::find_boards_by_usb(UsbId id);`

This is the single shared model the spec calls for — the uploader and the future emulator both read it, so the emulated Nano and the physical Nano cannot drift apart. `baud_rates` is a *list* specifically to encode the 115200/57600 clone split as data rather than as special-case logic in the programmer.

- [ ] **Step 1: Write the failing test**

Create `tests/test_board.cpp`:

```cpp
#include "harness.h"
#include "ardio/board.h"
#include <string>

TEST(board_database_contains_nano) {
    const ardio::Board* b = ardio::find_board_by_id("nano");
    CHECK(b != nullptr);
    CHECK(b->mcu == "atmega328p");
    CHECK_EQ(int(b->page_size), 128);
    CHECK_EQ(int(b->flash_size), 32768);
    CHECK(b->protocol == ardio::Protocol::Stk500v1);
}

TEST(nano_signature_is_atmega328p) {
    const ardio::Board* b = ardio::find_board_by_id("nano");
    CHECK(b != nullptr);
    CHECK_EQ(int(b->signature[0]), 0x1E);
    CHECK_EQ(int(b->signature[1]), 0x95);
    CHECK_EQ(int(b->signature[2]), 0x0F);
}

TEST(nano_tries_both_bootloader_baud_rates_new_first) {
    const ardio::Board* b = ardio::find_board_by_id("nano");
    CHECK(b != nullptr);
    CHECK_EQ(b->baud_rates.size(), size_t(2));
    CHECK_EQ(b->baud_rates[0], 115200);
    CHECK_EQ(b->baud_rates[1], 57600);
}

TEST(ch340_usb_id_identifies_nano) {
    auto matches = ardio::find_boards_by_usb({0x1A86, 0x7523});
    CHECK_EQ(matches.size(), size_t(1));
    CHECK(matches[0]->id == "nano");
}

TEST(unknown_usb_id_matches_nothing) {
    auto matches = ardio::find_boards_by_usb({0xDEAD, 0xBEEF});
    CHECK_EQ(matches.size(), size_t(0));
}

TEST(unknown_board_id_returns_null) {
    CHECK(ardio::find_board_by_id("teapot") == nullptr);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd ~/ardio && cmake --build build 2>&1 | head -5`
Expected: FAIL — `'ardio/board.h' file not found`.

- [ ] **Step 3: Write minimal implementation**

Create `include/ardio/board.h`:

```cpp
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ardio {

enum class Protocol { Stk500v1 };

struct UsbId {
    uint16_t vid = 0;
    uint16_t pid = 0;
    bool operator==(const UsbId&) const = default;
};

struct Board {
    std::string id;             // "nano"
    std::string name;           // "Arduino Nano (ATmega328P)"
    std::string mcu;            // "atmega328p"
    Protocol protocol = Protocol::Stk500v1;
    std::vector<UsbId> usb_ids; // USB-serial bridges this board ships with
    std::vector<int> baud_rates;// bootloader baud, tried in order
    uint32_t flash_size = 0;
    uint32_t page_size = 0;
    std::array<uint8_t, 3> signature{};
    std::string gcc_mcu;        // value for avr-gcc -mmcu=
    int f_cpu = 0;
};

const std::vector<Board>& board_database();
const Board* find_board_by_id(std::string_view id);
std::vector<const Board*> find_boards_by_usb(UsbId id);

} // namespace ardio
```

Create `src/board.cpp`:

```cpp
#include "ardio/board.h"

namespace ardio {

const std::vector<Board>& board_database() {
    static const std::vector<Board> db = {
        Board{
            .id = "nano",
            .name = "Arduino Nano (ATmega328P)",
            .mcu = "atmega328p",
            .protocol = Protocol::Stk500v1,
            // 1A86:7523 CH340, 0403:6001 FTDI FT232R, 2341:0043 genuine Arduino
            .usb_ids = {{0x1A86, 0x7523}, {0x0403, 0x6001}, {0x2341, 0x0043}},
            // New bootloader first; old-bootloader clones fall back to 57600.
            .baud_rates = {115200, 57600},
            .flash_size = 32768,
            .page_size = 128,
            .signature = {0x1E, 0x95, 0x0F},
            .gcc_mcu = "atmega328p",
            .f_cpu = 16000000,
        },
    };
    return db;
}

const Board* find_board_by_id(std::string_view id) {
    for (const Board& b : board_database())
        if (b.id == id) return &b;
    return nullptr;
}

std::vector<const Board*> find_boards_by_usb(UsbId id) {
    std::vector<const Board*> out;
    for (const Board& b : board_database())
        for (const UsbId& u : b.usb_ids)
            if (u == id) { out.push_back(&b); break; }
    return out;
}

} // namespace ardio
```

Modify `CMakeLists.txt`:

```cmake
target_sources(libardio PRIVATE src/hex.cpp src/board.cpp)

add_executable(ardio_tests tests/main.cpp tests/test_harness.cpp tests/test_hex.cpp
                           tests/test_board.cpp)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd ~/ardio && cmake --build build && ./build/ardio_tests`
Expected: PASS — 11 tests, 0 failures.

- [ ] **Step 5: Commit**

```bash
cd ~/ardio
git add include/ardio/board.h src/board.cpp tests/test_board.cpp CMakeLists.txt
git commit -m "feat: add shared board description model with Nano entry"
```

---

### Task 4: STK500v1 protocol encoders

**Files:**
- Create: `include/ardio/protocol/stk500v1.h`
- Create: `src/protocol/stk500v1.cpp`
- Create: `tests/test_stk500v1.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces (all in `namespace ardio::stk500v1`):
  - Constants `kOk = 0x10`, `kInSync = 0x14`, `kCrcEop = 0x20`.
  - `std::vector<uint8_t> cmd_get_sync();`
  - `std::vector<uint8_t> cmd_enter_progmode();`
  - `std::vector<uint8_t> cmd_leave_progmode();`
  - `std::vector<uint8_t> cmd_read_signature();`
  - `std::vector<uint8_t> cmd_load_address(uint16_t word_address);`
  - `std::vector<uint8_t> cmd_prog_page(const uint8_t* data, uint16_t length);`
  - `std::vector<uint8_t> cmd_read_page(uint16_t length);`
  - `bool is_ok_response(const std::vector<uint8_t>& resp);`

This is the heart of "ardio owns every byte on the wire". These are pure functions — bytes in, bytes out — so the entire protocol is tested without a board attached. Two details that are easy to get wrong and are pinned by tests: the load-address argument is a **word** address (little-endian), not a byte address, while the page-length argument in `prog_page` is a **byte** count sent **big-endian**. Mixing these up is the classic STK500 bug.

- [ ] **Step 1: Write the failing test**

Create `tests/test_stk500v1.cpp`:

```cpp
#include "harness.h"
#include "ardio/protocol/stk500v1.h"

using namespace ardio::stk500v1;

TEST(stk_get_sync_is_0x30_then_eop) {
    auto c = cmd_get_sync();
    CHECK_EQ(c.size(), size_t(2));
    CHECK_EQ(int(c[0]), 0x30);
    CHECK_EQ(int(c[1]), 0x20);
}

TEST(stk_enter_and_leave_progmode) {
    auto enter = cmd_enter_progmode();
    CHECK_EQ(int(enter[0]), 0x50);
    CHECK_EQ(int(enter[1]), 0x20);
    auto leave = cmd_leave_progmode();
    CHECK_EQ(int(leave[0]), 0x51);
    CHECK_EQ(int(leave[1]), 0x20);
}

TEST(stk_read_signature_is_0x75) {
    auto c = cmd_read_signature();
    CHECK_EQ(int(c[0]), 0x75);
    CHECK_EQ(int(c[1]), 0x20);
}

TEST(stk_load_address_sends_word_address_little_endian) {
    // byte address 0x0100 is word address 0x0080
    auto c = cmd_load_address(0x0080);
    CHECK_EQ(c.size(), size_t(4));
    CHECK_EQ(int(c[0]), 0x55);
    CHECK_EQ(int(c[1]), 0x80);  // low byte first
    CHECK_EQ(int(c[2]), 0x00);
    CHECK_EQ(int(c[3]), 0x20);
}

TEST(stk_load_address_high_byte) {
    auto c = cmd_load_address(0x1234);
    CHECK_EQ(int(c[1]), 0x34);
    CHECK_EQ(int(c[2]), 0x12);
}

TEST(stk_prog_page_sends_byte_length_big_endian_and_flash_marker) {
    uint8_t data[128];
    for (int i = 0; i < 128; ++i) data[i] = uint8_t(i);
    auto c = cmd_prog_page(data, 128);
    // 0x64, len_hi, len_lo, 'F', <128 bytes>, 0x20
    CHECK_EQ(c.size(), size_t(5 + 128));
    CHECK_EQ(int(c[0]), 0x64);
    CHECK_EQ(int(c[1]), 0x00);  // length high byte first
    CHECK_EQ(int(c[2]), 128);
    CHECK_EQ(int(c[3]), 'F');
    CHECK_EQ(int(c[4]), 0);
    CHECK_EQ(int(c[131]), 127);
    CHECK_EQ(int(c.back()), 0x20);
}

TEST(stk_read_page_requests_flash) {
    auto c = cmd_read_page(128);
    CHECK_EQ(c.size(), size_t(5));
    CHECK_EQ(int(c[0]), 0x74);
    CHECK_EQ(int(c[1]), 0x00);
    CHECK_EQ(int(c[2]), 128);
    CHECK_EQ(int(c[3]), 'F');
    CHECK_EQ(int(c[4]), 0x20);
}

TEST(ok_response_is_insync_then_ok) {
    CHECK(is_ok_response({0x14, 0x10}));
    CHECK(!is_ok_response({0x14, 0x11}));
    CHECK(!is_ok_response({0x00, 0x10}));
    CHECK(!is_ok_response({0x14}));
    CHECK(!is_ok_response({}));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd ~/ardio && cmake --build build 2>&1 | head -5`
Expected: FAIL — `'ardio/protocol/stk500v1.h' file not found`.

- [ ] **Step 3: Write minimal implementation**

Create `include/ardio/protocol/stk500v1.h`:

```cpp
#pragma once
#include <cstdint>
#include <vector>

namespace ardio::stk500v1 {

// Response bytes
inline constexpr uint8_t kInSync = 0x14;
inline constexpr uint8_t kOk     = 0x10;
inline constexpr uint8_t kNoSync = 0x15;
// Terminator appended to every command
inline constexpr uint8_t kCrcEop = 0x20;

std::vector<uint8_t> cmd_get_sync();
std::vector<uint8_t> cmd_enter_progmode();
std::vector<uint8_t> cmd_leave_progmode();
std::vector<uint8_t> cmd_read_signature();

// `word_address` is a WORD address (byte address / 2), sent little-endian.
std::vector<uint8_t> cmd_load_address(uint16_t word_address);

// `length` is a BYTE count, sent big-endian. 'F' selects flash (vs 'E' EEPROM).
std::vector<uint8_t> cmd_prog_page(const uint8_t* data, uint16_t length);
std::vector<uint8_t> cmd_read_page(uint16_t length);

bool is_ok_response(const std::vector<uint8_t>& resp);

} // namespace ardio::stk500v1
```

Create `src/protocol/stk500v1.cpp`:

```cpp
#include "ardio/protocol/stk500v1.h"

namespace ardio::stk500v1 {
namespace {
constexpr uint8_t kGetSync       = 0x30;
constexpr uint8_t kEnterProgmode = 0x50;
constexpr uint8_t kLeaveProgmode = 0x51;
constexpr uint8_t kLoadAddress   = 0x55;
constexpr uint8_t kProgPage      = 0x64;
constexpr uint8_t kReadPage      = 0x74;
constexpr uint8_t kReadSign      = 0x75;
constexpr uint8_t kMemFlash      = 'F';
} // namespace

std::vector<uint8_t> cmd_get_sync()        { return {kGetSync, kCrcEop}; }
std::vector<uint8_t> cmd_enter_progmode()  { return {kEnterProgmode, kCrcEop}; }
std::vector<uint8_t> cmd_leave_progmode()  { return {kLeaveProgmode, kCrcEop}; }
std::vector<uint8_t> cmd_read_signature()  { return {kReadSign, kCrcEop}; }

std::vector<uint8_t> cmd_load_address(uint16_t word_address) {
    return {kLoadAddress,
            uint8_t(word_address & 0xFF),
            uint8_t((word_address >> 8) & 0xFF),
            kCrcEop};
}

std::vector<uint8_t> cmd_prog_page(const uint8_t* data, uint16_t length) {
    std::vector<uint8_t> out;
    out.reserve(size_t(length) + 5);
    out.push_back(kProgPage);
    out.push_back(uint8_t((length >> 8) & 0xFF));  // big-endian
    out.push_back(uint8_t(length & 0xFF));
    out.push_back(kMemFlash);
    out.insert(out.end(), data, data + length);
    out.push_back(kCrcEop);
    return out;
}

std::vector<uint8_t> cmd_read_page(uint16_t length) {
    return {kReadPage,
            uint8_t((length >> 8) & 0xFF),
            uint8_t(length & 0xFF),
            kMemFlash,
            kCrcEop};
}

bool is_ok_response(const std::vector<uint8_t>& resp) {
    return resp.size() == 2 && resp[0] == kInSync && resp[1] == kOk;
}

} // namespace ardio::stk500v1
```

Modify `CMakeLists.txt`:

```cmake
target_sources(libardio PRIVATE src/hex.cpp src/board.cpp src/protocol/stk500v1.cpp)

add_executable(ardio_tests tests/main.cpp tests/test_harness.cpp tests/test_hex.cpp
                           tests/test_board.cpp tests/test_stk500v1.cpp)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd ~/ardio && cmake --build build && ./build/ardio_tests`
Expected: PASS — 19 tests, 0 failures.

- [ ] **Step 5: Commit**

```bash
cd ~/ardio
git add include/ardio/protocol src/protocol tests/test_stk500v1.cpp CMakeLists.txt
git commit -m "feat: add STK500v1 command encoders"
```

---

### Task 5: Serial port interface and macOS implementation

**Files:**
- Create: `include/ardio/platform/serial.h`
- Create: `src/platform/macos/serial_macos.cpp`
- Create: `tests/test_serial_fake.cpp`
- Create: `include/ardio/platform/fake_serial.h`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `class ardio::SerialPort` — abstract: `virtual bool open(const std::string& path, int baud, std::string& error)`, `virtual void close()`, `virtual bool write(const uint8_t* data, size_t len)`, `virtual size_t read(uint8_t* out, size_t max, int timeout_ms)`, `virtual void set_dtr(bool)`, `virtual void set_rts(bool)`, `virtual ~SerialPort()`.
  - `std::unique_ptr<SerialPort> ardio::make_serial_port();`
  - `class ardio::FakeSerialPort : public SerialPort` — records writes in `written`, replays `to_read`, records `dtr_history`. Used by Task 6's tests.

The abstract interface is what lets the programmer in Task 6 be tested with no hardware. `make_serial_port()` is the only function whose implementation is platform-specific.

- [ ] **Step 1: Write the failing test**

Create `tests/test_serial_fake.cpp`:

```cpp
#include "harness.h"
#include "ardio/platform/fake_serial.h"
#include <string>

TEST(fake_serial_records_writes) {
    ardio::FakeSerialPort port;
    std::string err;
    CHECK(port.open("/dev/fake", 115200, err));
    uint8_t data[] = {0x30, 0x20};
    CHECK(port.write(data, 2));
    CHECK_EQ(port.written.size(), size_t(2));
    CHECK_EQ(int(port.written[0]), 0x30);
}

TEST(fake_serial_replays_queued_reads) {
    ardio::FakeSerialPort port;
    port.to_read = {0x14, 0x10};
    uint8_t buf[8];
    size_t n = port.read(buf, 8, 100);
    CHECK_EQ(n, size_t(2));
    CHECK_EQ(int(buf[0]), 0x14);
    CHECK_EQ(int(buf[1]), 0x10);
}

TEST(fake_serial_read_returns_zero_when_empty) {
    ardio::FakeSerialPort port;
    uint8_t buf[8];
    CHECK_EQ(port.read(buf, 8, 10), size_t(0));
}

TEST(fake_serial_records_dtr_toggles) {
    ardio::FakeSerialPort port;
    port.set_dtr(false);
    port.set_dtr(true);
    CHECK_EQ(port.dtr_history.size(), size_t(2));
    CHECK_EQ(int(port.dtr_history[0]), 0);
    CHECK_EQ(int(port.dtr_history[1]), 1);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd ~/ardio && cmake --build build 2>&1 | head -5`
Expected: FAIL — `'ardio/platform/fake_serial.h' file not found`.

- [ ] **Step 3: Write minimal implementation**

Create `include/ardio/platform/serial.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

namespace ardio {

class SerialPort {
public:
    virtual ~SerialPort() = default;
    virtual bool open(const std::string& path, int baud, std::string& error) = 0;
    virtual void close() = 0;
    virtual bool write(const uint8_t* data, size_t len) = 0;
    // Returns bytes read; 0 on timeout.
    virtual size_t read(uint8_t* out, size_t max, int timeout_ms) = 0;
    virtual void set_dtr(bool level) = 0;
    virtual void set_rts(bool level) = 0;
};

std::unique_ptr<SerialPort> make_serial_port();

} // namespace ardio
```

Create `include/ardio/platform/fake_serial.h`:

```cpp
#pragma once
#include "ardio/platform/serial.h"
#include <algorithm>
#include <deque>
#include <vector>

namespace ardio {

// In-memory SerialPort for tests. Records everything written and DTR/RTS
// changes, and replays bytes queued in `to_read`.
class FakeSerialPort : public SerialPort {
public:
    std::vector<uint8_t> written;
    std::deque<uint8_t> to_read;
    std::vector<bool> dtr_history;
    std::vector<bool> rts_history;
    bool is_open = false;
    int opened_baud = 0;
    std::string opened_path;

    bool open(const std::string& path, int baud, std::string&) override {
        opened_path = path;
        opened_baud = baud;
        is_open = true;
        return true;
    }
    void close() override { is_open = false; }

    bool write(const uint8_t* data, size_t len) override {
        written.insert(written.end(), data, data + len);
        return true;
    }

    size_t read(uint8_t* out, size_t max, int) override {
        size_t n = std::min(max, to_read.size());
        for (size_t i = 0; i < n; ++i) { out[i] = to_read.front(); to_read.pop_front(); }
        return n;
    }

    void set_dtr(bool level) override { dtr_history.push_back(level); }
    void set_rts(bool level) override { rts_history.push_back(level); }
};

} // namespace ardio
```

Create `src/platform/macos/serial_macos.cpp`:

```cpp
#include "ardio/platform/serial.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <IOKit/serial/ioss.h>
#include <cerrno>
#include <cstring>
#include <poll.h>

namespace ardio {
namespace {

class MacSerialPort : public SerialPort {
public:
    ~MacSerialPort() override { close(); }

    bool open(const std::string& path, int baud, std::string& error) override {
        close();
        fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) {
            error = "cannot open " + path + ": " + std::strerror(errno);
            return false;
        }

        termios tty{};
        if (tcgetattr(fd_, &tty) != 0) {
            error = "tcgetattr failed on " + path + ": " + std::strerror(errno);
            close();
            return false;
        }

        cfmakeraw(&tty);
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~unsigned(CSTOPB);   // 1 stop bit
        tty.c_cflag &= ~unsigned(CRTSCTS);  // no hardware flow control
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 0;

        if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
            error = "tcsetattr failed on " + path + ": " + std::strerror(errno);
            close();
            return false;
        }

        // IOSSIOSPEED sets arbitrary baud rates, including ones with no Bxxx
        // constant. Must be applied after tcsetattr, which would reset it.
        speed_t speed = speed_t(baud);
        if (ioctl(fd_, IOSSIOSPEED, &speed) == -1) {
            error = "cannot set baud " + std::to_string(baud) + " on " + path +
                    ": " + std::strerror(errno);
            close();
            return false;
        }

        tcflush(fd_, TCIOFLUSH);
        return true;
    }

    void close() override {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    bool write(const uint8_t* data, size_t len) override {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::write(fd_, data + sent, len - sent);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                return false;
            }
            sent += size_t(n);
        }
        return true;
    }

    size_t read(uint8_t* out, size_t max, int timeout_ms) override {
        pollfd pfd{fd_, POLLIN, 0};
        size_t got = 0;
        while (got < max) {
            int rc = ::poll(&pfd, 1, timeout_ms);
            if (rc <= 0) break;                 // timeout or error
            ssize_t n = ::read(fd_, out + got, max - got);
            if (n <= 0) break;
            got += size_t(n);
        }
        return got;
    }

    void set_dtr(bool level) override { set_line(TIOCM_DTR, level); }
    void set_rts(bool level) override { set_line(TIOCM_RTS, level); }

private:
    void set_line(int flag, bool level) {
        if (fd_ < 0) return;
        // TIOCMBIS asserts the line (logic low on the wire), TIOCMBIC releases it.
        ioctl(fd_, level ? TIOCMBIS : TIOCMBIC, &flag);
    }

    int fd_ = -1;
};

} // namespace

std::unique_ptr<SerialPort> make_serial_port() {
    return std::make_unique<MacSerialPort>();
}

} // namespace ardio
```

Modify `CMakeLists.txt` — add the platform source and link IOKit:

```cmake
target_sources(libardio PRIVATE src/hex.cpp src/board.cpp src/protocol/stk500v1.cpp)

if(APPLE)
    target_sources(libardio PRIVATE src/platform/macos/serial_macos.cpp)
    target_link_libraries(libardio PUBLIC "-framework IOKit" "-framework CoreFoundation")
endif()

add_executable(ardio_tests tests/main.cpp tests/test_harness.cpp tests/test_hex.cpp
                           tests/test_board.cpp tests/test_stk500v1.cpp
                           tests/test_serial_fake.cpp)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd ~/ardio && cmake --build build && ./build/ardio_tests`
Expected: PASS — 23 tests, 0 failures.

- [ ] **Step 5: Commit**

```bash
cd ~/ardio
git add include/ardio/platform src/platform tests/test_serial_fake.cpp CMakeLists.txt
git commit -m "feat: add serial port interface with macOS termios implementation"
```

---

### Task 6: STK500v1 programmer

**Files:**
- Create: `include/ardio/protocol/programmer.h`
- Create: `src/protocol/programmer.cpp`
- Create: `tests/test_programmer.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SerialPort`/`FakeSerialPort` (Task 5), `stk500v1::*` encoders (Task 4), `Board` (Task 3), `HexImage` (Task 2).
- Produces:
  - `struct ardio::UploadResult { bool ok; std::string error; int baud_used; std::string stage; };`
  - `using ardio::ProgressFn = std::function<void(const std::string&)>;`
  - `ardio::UploadResult ardio::upload_stk500v1(SerialPort& port, const std::string& path, const Board& board, const HexImage& image, const ProgressFn& progress);` — pass `nullptr` for no progress reporting.

This is where the spec's error-handling requirement lands: `UploadResult::stage` names the protocol phase that failed (`"sync"`, `"signature"`, `"write"`, `"verify"`) rather than returning a generic failure, and `baud_used` reports which of the board's baud rates actually worked so the user learns which bootloader their clone has.

The reset sequence: toggling DTR low then high pulses the ATmega's RESET line through the bootloader's auto-reset capacitor, dropping the chip into the bootloader.

- [ ] **Step 1: Write the failing test**

Create `tests/test_programmer.cpp`:

```cpp
#include "harness.h"
#include "ardio/protocol/programmer.h"
#include "ardio/platform/fake_serial.h"
#include "ardio/board.h"
#include <string>

namespace {

// Queues the responses a healthy bootloader gives for a full upload of
// `pages` flash pages: sync, enter progmode, signature, then per page a
// load-address ack and a prog-page ack, then leave progmode.
void queue_successful_session(ardio::FakeSerialPort& port, int pages,
                              const std::array<uint8_t, 3>& sig) {
    auto ok = [&] { port.to_read.push_back(0x14); port.to_read.push_back(0x10); };
    ok();                                    // get_sync
    ok();                                    // enter progmode
    port.to_read.push_back(0x14);            // signature
    port.to_read.push_back(sig[0]);
    port.to_read.push_back(sig[1]);
    port.to_read.push_back(sig[2]);
    port.to_read.push_back(0x10);
    for (int i = 0; i < pages; ++i) { ok(); ok(); }  // load address + prog page
    ok();                                    // leave progmode
}

ardio::HexImage one_page_image() {
    ardio::HexImage img;
    img.data.assign(128, 0xAB);
    return img;
}

} // namespace

TEST(upload_succeeds_on_healthy_bootloader) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    CHECK(nano != nullptr);
    ardio::FakeSerialPort port;
    queue_successful_session(port, 1, nano->signature);

    auto result = ardio::upload_stk500v1(port, "/dev/fake", *nano, one_page_image(), nullptr);
    CHECK(result.ok);
    CHECK_EQ(result.baud_used, 115200);
}

TEST(upload_pulses_dtr_to_reset_board) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    ardio::FakeSerialPort port;
    queue_successful_session(port, 1, nano->signature);

    ardio::upload_stk500v1(port, "/dev/fake", *nano, one_page_image(), nullptr);
    CHECK(port.dtr_history.size() >= 2);
    CHECK_EQ(int(port.dtr_history[0]), 1);   // assert reset
    CHECK_EQ(int(port.dtr_history[1]), 0);   // release
}

TEST(upload_reports_sync_stage_when_bootloader_silent) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    ardio::FakeSerialPort port;   // no queued responses at all

    auto result = ardio::upload_stk500v1(port, "/dev/fake", *nano, one_page_image(), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "sync");
    CHECK(result.error.find("57600") != std::string::npos);  // mentions both baud attempts
}

TEST(upload_reports_signature_mismatch_with_both_values) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    ardio::FakeSerialPort port;
    queue_successful_session(port, 1, {0x1E, 0x95, 0x16});  // ATmega32U4, wrong chip

    auto result = ardio::upload_stk500v1(port, "/dev/fake", *nano, one_page_image(), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "signature");
    CHECK(result.error.find("1e950f") != std::string::npos);
    CHECK(result.error.find("1e9516") != std::string::npos);
}

TEST(upload_splits_image_into_page_sized_writes) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    ardio::FakeSerialPort port;
    ardio::HexImage img;
    img.data.assign(300, 0x5A);          // 3 pages: 128 + 128 + 44
    queue_successful_session(port, 3, nano->signature);

    auto result = ardio::upload_stk500v1(port, "/dev/fake", *nano, img, nullptr);
    CHECK(result.ok);
    // Count 0x64 (prog page) commands issued.
    int prog_pages = 0;
    for (size_t i = 0; i < port.written.size(); ++i)
        if (port.written[i] == 0x64) { ++prog_pages; i += 4 + 128; }
    CHECK_EQ(prog_pages, 3);
}

TEST(upload_rejects_image_larger_than_flash) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    ardio::FakeSerialPort port;
    ardio::HexImage img;
    img.data.assign(40000, 0x00);        // > 32768

    auto result = ardio::upload_stk500v1(port, "/dev/fake", *nano, img, nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "size");
    CHECK(result.error.find("32768") != std::string::npos);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd ~/ardio && cmake --build build 2>&1 | head -5`
Expected: FAIL — `'ardio/protocol/programmer.h' file not found`.

- [ ] **Step 3: Write minimal implementation**

Create `include/ardio/protocol/programmer.h`:

```cpp
#pragma once
#include "ardio/board.h"
#include "ardio/hex.h"
#include "ardio/platform/serial.h"
#include <functional>
#include <string>

namespace ardio {

struct UploadResult {
    bool ok = false;
    std::string error;
    int baud_used = 0;
    // Which protocol phase failed: "open", "sync", "signature", "size",
    // "write", "verify". Empty on success.
    std::string stage;
};

using ProgressFn = std::function<void(const std::string&)>;

// Resets the board via DTR, syncs with the bootloader (trying each baud rate
// in board.baud_rates), verifies the device signature, and writes the image
// page by page.
UploadResult upload_stk500v1(SerialPort& port, const std::string& path,
                             const Board& board, const HexImage& image,
                             const ProgressFn& progress);

} // namespace ardio
```

Create `src/protocol/programmer.cpp`:

```cpp
#include "ardio/protocol/programmer.h"
#include "ardio/protocol/stk500v1.h"

#include <chrono>
#include <thread>

namespace ardio {
namespace {

constexpr int kResponseTimeoutMs = 500;

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string hex3(const std::array<uint8_t, 3>& sig) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : sig) { s += d[b >> 4]; s += d[b & 0xF]; }
    return s;
}

// Sends a command and reads exactly `expected` response bytes.
std::vector<uint8_t> transact(SerialPort& port, const std::vector<uint8_t>& cmd,
                              size_t expected) {
    if (!port.write(cmd.data(), cmd.size())) return {};
    std::vector<uint8_t> resp(expected);
    size_t n = port.read(resp.data(), expected, kResponseTimeoutMs);
    resp.resize(n);
    return resp;
}

// Pulses DTR to yank RESET through the auto-reset capacitor, dropping the
// ATmega into its bootloader.
void pulse_reset(SerialPort& port) {
    port.set_dtr(true);
    port.set_rts(true);
    sleep_ms(50);
    port.set_dtr(false);
    port.set_rts(false);
    sleep_ms(50);
}

bool try_sync(SerialPort& port) {
    // The bootloader may have stale bytes buffered; a few attempts settles it.
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto resp = transact(port, stk500v1::cmd_get_sync(), 2);
        if (stk500v1::is_ok_response(resp)) return true;
        sleep_ms(50);
    }
    return false;
}

} // namespace

UploadResult upload_stk500v1(SerialPort& port, const std::string& path,
                             const Board& board, const HexImage& image,
                             const ProgressFn& progress) {
    UploadResult result;
    auto report = [&](const std::string& msg) { if (progress) progress(msg); };

    if (image.data.size() > board.flash_size) {
        result.stage = "size";
        result.error = "sketch is " + std::to_string(image.data.size()) +
                       " bytes but " + board.name + " has only " +
                       std::to_string(board.flash_size) + " bytes of flash";
        return result;
    }

    // Try each bootloader baud rate. Nano clones split between 115200 (new
    // bootloader) and 57600 (old); the user should never need to know which.
    std::string tried;
    bool synced = false;
    for (int baud : board.baud_rates) {
        if (!tried.empty()) tried += ", ";
        tried += std::to_string(baud);

        std::string open_error;
        if (!port.open(path, baud, open_error)) {
            result.stage = "open";
            result.error = open_error;
            return result;
        }
        report("trying " + std::to_string(baud) + " baud");
        pulse_reset(port);
        if (try_sync(port)) {
            synced = true;
            result.baud_used = baud;
            break;
        }
        port.close();
    }

    if (!synced) {
        result.stage = "sync";
        result.error = "no response from bootloader on " + path +
                       " at any known baud rate (tried " + tried +
                       "). Check the board is plugged in and not held in reset.";
        return result;
    }
    report("synced at " + std::to_string(result.baud_used) + " baud");

    if (!stk500v1::is_ok_response(transact(port, stk500v1::cmd_enter_progmode(), 2))) {
        result.stage = "sync";
        result.error = "bootloader refused to enter programming mode";
        return result;
    }

    // Signature response is: INSYNC, sig0, sig1, sig2, OK
    auto sig_resp = transact(port, stk500v1::cmd_read_signature(), 5);
    if (sig_resp.size() != 5 || sig_resp[0] != stk500v1::kInSync ||
        sig_resp[4] != stk500v1::kOk) {
        result.stage = "signature";
        result.error = "could not read device signature";
        return result;
    }
    std::array<uint8_t, 3> got{sig_resp[1], sig_resp[2], sig_resp[3]};
    if (got != board.signature) {
        result.stage = "signature";
        result.error = "device signature mismatch: expected " + hex3(board.signature) +
                       " for " + board.name + " but found " + hex3(got) +
                       ". Wrong --board, or a different chip than expected.";
        return result;
    }
    report("signature ok (" + hex3(got) + ")");

    // Write the image one flash page at a time.
    const uint32_t page = board.page_size;
    for (uint32_t offset = 0; offset < image.data.size(); offset += page) {
        uint32_t chunk = uint32_t(image.data.size()) - offset;
        if (chunk > page) chunk = page;

        uint16_t word_addr = uint16_t((image.base_address + offset) / 2);
        if (!stk500v1::is_ok_response(
                transact(port, stk500v1::cmd_load_address(word_addr), 2))) {
            result.stage = "write";
            result.error = "bootloader rejected load-address at byte offset " +
                           std::to_string(offset);
            return result;
        }
        if (!stk500v1::is_ok_response(
                transact(port,
                         stk500v1::cmd_prog_page(image.data.data() + offset, uint16_t(chunk)),
                         2))) {
            result.stage = "write";
            result.error = "page write failed at byte offset " + std::to_string(offset);
            return result;
        }
        report("wrote " + std::to_string(offset + chunk) + "/" +
               std::to_string(image.data.size()) + " bytes");
    }

    if (!stk500v1::is_ok_response(transact(port, stk500v1::cmd_leave_progmode(), 2))) {
        result.stage = "write";
        result.error = "bootloader refused to leave programming mode";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ardio
```

Modify `CMakeLists.txt`:

```cmake
target_sources(libardio PRIVATE src/hex.cpp src/board.cpp src/protocol/stk500v1.cpp
                                src/protocol/programmer.cpp)

add_executable(ardio_tests tests/main.cpp tests/test_harness.cpp tests/test_hex.cpp
                           tests/test_board.cpp tests/test_stk500v1.cpp
                           tests/test_serial_fake.cpp tests/test_programmer.cpp)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd ~/ardio && cmake --build build && ./build/ardio_tests`
Expected: PASS — 29 tests, 0 failures.

Note: the sync-failure test exercises three retries at two baud rates with 50ms sleeps, so it takes roughly a second. That is expected.

- [ ] **Step 5: Commit**

```bash
cd ~/ardio
git add include/ardio/protocol/programmer.h src/protocol/programmer.cpp \
        tests/test_programmer.cpp CMakeLists.txt
git commit -m "feat: add STK500v1 programmer with baud fallback and staged errors"
```

---

### Task 7: macOS port enumeration

**Files:**
- Create: `include/ardio/platform/ports.h`
- Create: `src/platform/macos/ports_macos.cpp`
- Create: `tests/test_ports.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `UsbId`, `find_boards_by_usb` (Task 3).
- Produces:
  - `struct ardio::PortInfo { std::string device; std::string description; UsbId usb; bool has_usb_id; };`
  - `std::vector<PortInfo> ardio::enumerate_ports();`
  - `struct ardio::PortSelection { const PortInfo* port; const Board* board; std::string error; };`
  - `PortSelection ardio::select_port(const std::vector<PortInfo>& ports, std::string_view wanted_port, std::string_view wanted_board);`

`select_port` holds the auto-detection policy the spec requires, and it is a pure function over a port list — so every branch (no ports, one port, several, unknown VID/PID) is tested without hardware. `enumerate_ports()` is the only part that needs a real Mac.

- [ ] **Step 1: Write the failing test**

Create `tests/test_ports.cpp`:

```cpp
#include "harness.h"
#include "ardio/platform/ports.h"
#include <string>

namespace {
ardio::PortInfo nano_port(const char* dev) {
    return ardio::PortInfo{dev, "USB Serial", {0x1A86, 0x7523}, true};
}
ardio::PortInfo unknown_port(const char* dev) {
    return ardio::PortInfo{dev, "Some Device", {0x9999, 0x0001}, true};
}
} // namespace

TEST(select_port_auto_picks_the_single_known_board) {
    std::vector<ardio::PortInfo> ports{nano_port("/dev/cu.usbserial-110")};
    auto sel = ardio::select_port(ports, "", "");
    CHECK(sel.port != nullptr);
    CHECK(sel.board != nullptr);
    CHECK(sel.board->id == "nano");
    CHECK(sel.error.empty());
}

TEST(select_port_refuses_to_guess_between_two_boards) {
    std::vector<ardio::PortInfo> ports{nano_port("/dev/cu.usbserial-110"),
                                       nano_port("/dev/cu.usbserial-210")};
    auto sel = ardio::select_port(ports, "", "");
    CHECK(sel.port == nullptr);
    CHECK(sel.error.find("--port") != std::string::npos);
    CHECK(sel.error.find("usbserial-110") != std::string::npos);
    CHECK(sel.error.find("usbserial-210") != std::string::npos);
}

TEST(select_port_explicit_port_wins_over_ambiguity) {
    std::vector<ardio::PortInfo> ports{nano_port("/dev/cu.usbserial-110"),
                                       nano_port("/dev/cu.usbserial-210")};
    auto sel = ardio::select_port(ports, "/dev/cu.usbserial-210", "");
    CHECK(sel.port != nullptr);
    CHECK(sel.port->device == "/dev/cu.usbserial-210");
}

TEST(select_port_errors_when_explicit_port_absent) {
    std::vector<ardio::PortInfo> ports{nano_port("/dev/cu.usbserial-110")};
    auto sel = ardio::select_port(ports, "/dev/cu.nope", "");
    CHECK(sel.port == nullptr);
    CHECK(sel.error.find("/dev/cu.nope") != std::string::npos);
    CHECK(sel.error.find("usbserial-110") != std::string::npos);  // lists what exists
}

TEST(select_port_requires_explicit_board_for_unknown_usb_id) {
    std::vector<ardio::PortInfo> ports{unknown_port("/dev/cu.usbserial-999")};
    auto sel = ardio::select_port(ports, "", "");
    CHECK(sel.board == nullptr);
    CHECK(sel.error.find("--board") != std::string::npos);
}

TEST(select_port_accepts_explicit_board_for_unknown_usb_id) {
    std::vector<ardio::PortInfo> ports{unknown_port("/dev/cu.usbserial-999")};
    auto sel = ardio::select_port(ports, "", "nano");
    CHECK(sel.port != nullptr);
    CHECK(sel.board != nullptr);
    CHECK(sel.board->id == "nano");
    CHECK(sel.error.empty());
}

TEST(select_port_rejects_unknown_board_name) {
    std::vector<ardio::PortInfo> ports{nano_port("/dev/cu.usbserial-110")};
    auto sel = ardio::select_port(ports, "", "teapot");
    CHECK(sel.board == nullptr);
    CHECK(sel.error.find("teapot") != std::string::npos);
}

TEST(select_port_reports_no_ports_found) {
    std::vector<ardio::PortInfo> ports;
    auto sel = ardio::select_port(ports, "", "");
    CHECK(sel.port == nullptr);
    CHECK(sel.error.find("no serial ports") != std::string::npos);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd ~/ardio && cmake --build build 2>&1 | head -5`
Expected: FAIL — `'ardio/platform/ports.h' file not found`.

- [ ] **Step 3: Write minimal implementation**

Create `include/ardio/platform/ports.h`:

```cpp
#pragma once
#include "ardio/board.h"
#include <string>
#include <string_view>
#include <vector>

namespace ardio {

struct PortInfo {
    std::string device;       // "/dev/cu.usbserial-110"
    std::string description;  // "USB Serial"
    UsbId usb;
    bool has_usb_id = false;
};

// Platform-specific. Lists candidate serial ports with USB IDs where known.
std::vector<PortInfo> enumerate_ports();

struct PortSelection {
    const PortInfo* port = nullptr;
    const Board* board = nullptr;
    std::string error;
};

// Applies auto-detection policy. Never guesses between candidates: if the
// choice is ambiguous it returns an error naming the options.
PortSelection select_port(const std::vector<PortInfo>& ports,
                          std::string_view wanted_port,
                          std::string_view wanted_board);

} // namespace ardio
```

Create `src/platform/ports_select.cpp` (platform-independent policy):

```cpp
#include "ardio/platform/ports.h"

namespace ardio {
namespace {

std::string list_devices(const std::vector<PortInfo>& ports) {
    std::string s;
    for (const PortInfo& p : ports) {
        s += "\n  " + p.device;
        if (!p.description.empty()) s += "  (" + p.description + ")";
    }
    return s;
}

} // namespace

PortSelection select_port(const std::vector<PortInfo>& ports,
                          std::string_view wanted_port,
                          std::string_view wanted_board) {
    PortSelection sel;

    // An explicitly named board must exist, whatever else happens.
    const Board* forced_board = nullptr;
    if (!wanted_board.empty()) {
        forced_board = find_board_by_id(wanted_board);
        if (!forced_board) {
            sel.error = "unknown board '" + std::string(wanted_board) +
                        "'. Run 'ardio boards' to see supported targets.";
            return sel;
        }
    }

    if (ports.empty()) {
        sel.error = "no serial ports found. Is the board plugged in? "
                    "A CH340 board also needs its USB-serial driver installed.";
        return sel;
    }

    // Pick the port.
    if (!wanted_port.empty()) {
        for (const PortInfo& p : ports)
            if (p.device == wanted_port) { sel.port = &p; break; }
        if (!sel.port) {
            sel.error = "no such port '" + std::string(wanted_port) +
                        "'. Ports found:" + list_devices(ports);
            return sel;
        }
    } else {
        // Auto-detect: prefer ports whose USB ID matches a known board.
        std::vector<const PortInfo*> candidates;
        for (const PortInfo& p : ports)
            if (p.has_usb_id && !find_boards_by_usb(p.usb).empty())
                candidates.push_back(&p);

        if (candidates.size() == 1) {
            sel.port = candidates[0];
        } else if (candidates.size() > 1) {
            sel.error = "several boards connected -- pass --port to choose:" +
                        [&] {
                            std::string s;
                            for (const PortInfo* p : candidates)
                                s += "\n  " + p->device + "  (" + p->description + ")";
                            return s;
                        }();
            return sel;
        } else if (ports.size() == 1) {
            sel.port = &ports[0];   // sole port, unrecognised ID
        } else {
            sel.error = "could not identify a board -- pass --port to choose:" +
                        list_devices(ports);
            return sel;
        }
    }

    // Pick the board.
    if (forced_board) {
        sel.board = forced_board;
        return sel;
    }

    std::vector<const Board*> matches;
    if (sel.port->has_usb_id) matches = find_boards_by_usb(sel.port->usb);

    if (matches.size() == 1) {
        sel.board = matches[0];
    } else if (matches.size() > 1) {
        std::string names;
        for (const Board* b : matches) names += "\n  " + b->id + "  (" + b->name + ")";
        sel.error = "USB ID matches several boards -- pass --board:" + names;
        sel.port = nullptr;
    } else {
        sel.error = "unrecognised device on " + sel.port->device +
                    " -- pass --board to say what it is. "
                    "Run 'ardio boards' to see supported targets.";
        sel.port = nullptr;
    }
    return sel;
}

} // namespace ardio
```

Create `src/platform/macos/ports_macos.cpp`:

```cpp
#include "ardio/platform/ports.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#include <IOKit/usb/IOUSBLib.h>

namespace ardio {
namespace {

std::string cf_string_to_std(CFStringRef s) {
    if (!s) return {};
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) return buf;
    return {};
}

// Walks up the IORegistry from a serial service to the USB device that owns
// it, so we can read idVendor/idProduct. Not every serial port has one
// (Bluetooth ports, the debug console).
bool usb_ids_for_service(io_object_t service, UsbId& out, std::string& description) {
    io_registry_entry_t node = service;
    IOObjectRetain(node);

    bool found = false;
    for (int depth = 0; depth < 8 && node; ++depth) {
        CFTypeRef vid = IORegistryEntryCreateCFProperty(node, CFSTR("idVendor"),
                                                        kCFAllocatorDefault, 0);
        CFTypeRef pid = IORegistryEntryCreateCFProperty(node, CFSTR("idProduct"),
                                                        kCFAllocatorDefault, 0);
        if (vid && pid) {
            int v = 0, p = 0;
            CFNumberGetValue(CFNumberRef(vid), kCFNumberIntType, &v);
            CFNumberGetValue(CFNumberRef(pid), kCFNumberIntType, &p);
            out = UsbId{uint16_t(v), uint16_t(p)};
            CFTypeRef name = IORegistryEntryCreateCFProperty(
                node, CFSTR("USB Product Name"), kCFAllocatorDefault, 0);
            if (name) {
                description = cf_string_to_std(CFStringRef(name));
                CFRelease(name);
            }
            found = true;
        }
        if (vid) CFRelease(vid);
        if (pid) CFRelease(pid);
        if (found) break;

        io_registry_entry_t parent = 0;
        if (IORegistryEntryGetParentEntry(node, kIOServicePlane, &parent) != KERN_SUCCESS)
            break;
        IOObjectRelease(node);
        node = parent;
    }
    if (node) IOObjectRelease(node);
    return found;
}

} // namespace

std::vector<PortInfo> enumerate_ports() {
    std::vector<PortInfo> out;

    CFMutableDictionaryRef matching = IOServiceMatching(kIOSerialBSDServiceValue);
    if (!matching) return out;
    CFDictionarySetValue(matching, CFSTR(kIOSerialBSDTypeKey),
                         CFSTR(kIOSerialBSDAllTypes));

    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &it) != KERN_SUCCESS)
        return out;

    io_object_t service;
    while ((service = IOIteratorNext(it))) {
        CFTypeRef path = IORegistryEntryCreateCFProperty(
            service, CFSTR(kIOCalloutDeviceKey), kCFAllocatorDefault, 0);
        if (path) {
            PortInfo info;
            info.device = cf_string_to_std(CFStringRef(path));
            CFRelease(path);

            // Skip the ports that are never a board.
            if (info.device.find("Bluetooth") == std::string::npos &&
                info.device.find("debug-console") == std::string::npos) {
                info.has_usb_id = usb_ids_for_service(service, info.usb, info.description);
                out.push_back(info);
            }
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(it);
    return out;
}

} // namespace ardio
```

Modify `CMakeLists.txt`:

```cmake
target_sources(libardio PRIVATE src/hex.cpp src/board.cpp src/protocol/stk500v1.cpp
                                src/protocol/programmer.cpp src/platform/ports_select.cpp)

if(APPLE)
    target_sources(libardio PRIVATE src/platform/macos/serial_macos.cpp
                                    src/platform/macos/ports_macos.cpp)
    target_link_libraries(libardio PUBLIC "-framework IOKit" "-framework CoreFoundation")
endif()

add_executable(ardio_tests tests/main.cpp tests/test_harness.cpp tests/test_hex.cpp
                           tests/test_board.cpp tests/test_stk500v1.cpp
                           tests/test_serial_fake.cpp tests/test_programmer.cpp
                           tests/test_ports.cpp)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd ~/ardio && cmake --build build && ./build/ardio_tests`
Expected: PASS — 37 tests, 0 failures.

- [ ] **Step 5: Commit**

```bash
cd ~/ardio
git add include/ardio/platform/ports.h src/platform tests/test_ports.cpp CMakeLists.txt
git commit -m "feat: add port enumeration and auto-detection policy"
```

---

### Task 8: Config file and toolchain search path

**Files:**
- Create: `include/ardio/toolchain.h`
- Create: `src/toolchain/config.cpp`
- Create: `src/toolchain/search.cpp`
- Create: `tests/test_config.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct ardio::Config { std::vector<std::string> search_roots; std::string default_board; std::string default_port; int monitor_baud; };`
  - `Config ardio::parse_config(std::string_view text, std::string& error);`
  - `std::vector<std::string> ardio::default_search_roots();`
  - `struct ardio::ToolLocation { std::string path; std::string found_in_root; bool found; };`
  - `ToolLocation ardio::find_tool(std::string_view name, const std::vector<std::string>& roots);`

The config format is a deliberately small TOML subset — `key = value`, `key = ["a", "b"]`, `# comment` — because the spec forbids third-party libraries and a full TOML parser is not worth writing for six settings. Anything outside the subset is a clear error rather than silently ignored.

- [ ] **Step 1: Write the failing test**

Create `tests/test_config.cpp`:

```cpp
#include "harness.h"
#include "ardio/toolchain.h"
#include <string>

TEST(config_parses_string_and_int_values) {
    std::string err;
    auto cfg = ardio::parse_config(
        "default_board = \"nano\"\n"
        "monitor_baud = 9600\n", err);
    CHECK(err.empty());
    CHECK(cfg.default_board == "nano");
    CHECK_EQ(cfg.monitor_baud, 9600);
}

TEST(config_parses_search_root_array) {
    std::string err;
    auto cfg = ardio::parse_config(
        "search_roots = [\"/opt/mytools\", \"~/.ardio/tools\"]\n", err);
    CHECK(err.empty());
    CHECK_EQ(cfg.search_roots.size(), size_t(2));
    CHECK(cfg.search_roots[0] == "/opt/mytools");
    CHECK(cfg.search_roots[1] == "~/.ardio/tools");
}

TEST(config_ignores_comments_and_blank_lines) {
    std::string err;
    auto cfg = ardio::parse_config(
        "# a comment\n"
        "\n"
        "default_port = \"/dev/cu.usbserial-110\"   # trailing comment\n", err);
    CHECK(err.empty());
    CHECK(cfg.default_port == "/dev/cu.usbserial-110");
}

TEST(config_rejects_unknown_key) {
    std::string err;
    ardio::parse_config("nonsense = \"x\"\n", err);
    CHECK(err.find("nonsense") != std::string::npos);
    CHECK(err.find("line 1") != std::string::npos);
}

TEST(config_rejects_malformed_line) {
    std::string err;
    ardio::parse_config("default_board\n", err);
    CHECK(err.find("line 1") != std::string::npos);
}

TEST(config_defaults_are_sane_when_empty) {
    std::string err;
    auto cfg = ardio::parse_config("", err);
    CHECK(err.empty());
    CHECK_EQ(cfg.monitor_baud, 9600);
    CHECK(cfg.default_board.empty());
}

TEST(default_search_roots_are_ordered_ardio_first_path_last) {
    auto roots = ardio::default_search_roots();
    CHECK(roots.size() >= 4);
    CHECK(roots.front().find(".ardio") != std::string::npos);
    CHECK(roots.back() == "$PATH");
}

TEST(find_tool_reports_not_found_without_crashing) {
    auto loc = ardio::find_tool("definitely-not-a-real-tool-xyz",
                                {"/nonexistent-root-abc"});
    CHECK(!loc.found);
    CHECK(loc.path.empty());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd ~/ardio && cmake --build build 2>&1 | head -5`
Expected: FAIL — `'ardio/toolchain.h' file not found`.

- [ ] **Step 3: Write minimal implementation**

Create `include/ardio/toolchain.h`:

```cpp
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace ardio {

struct Config {
    std::vector<std::string> search_roots;  // empty = use defaults
    std::string default_board;
    std::string default_port;
    int monitor_baud = 9600;
};

// Parses ardio's config subset: `key = "string"`, `key = 123`,
// `key = ["a", "b"]`, `# comments`. Unknown keys are an error.
Config parse_config(std::string_view text, std::string& error);

// Ordered roots searched for toolchains and uploaders. "$PATH" is a sentinel
// meaning "search the PATH environment variable".
std::vector<std::string> default_search_roots();

struct ToolLocation {
    std::string path;           // absolute path to the executable
    std::string found_in_root;  // which root it came from, for `ardio doctor`
    bool found = false;
};

// Searches `roots` in order for an executable named `name`.
ToolLocation find_tool(std::string_view name, const std::vector<std::string>& roots);

} // namespace ardio
```

Create `src/toolchain/config.cpp`:

```cpp
#include "ardio/toolchain.h"
#include <cstdlib>

namespace ardio {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

// Strips a trailing `# comment`, but not a '#' inside a quoted string.
std::string_view strip_comment(std::string_view s) {
    bool in_string = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') in_string = !in_string;
        else if (s[i] == '#' && !in_string) return s.substr(0, i);
    }
    return s;
}

bool unquote(std::string_view v, std::string& out) {
    v = trim(v);
    if (v.size() < 2 || v.front() != '"' || v.back() != '"') return false;
    out = std::string(v.substr(1, v.size() - 2));
    return true;
}

bool parse_array(std::string_view v, std::vector<std::string>& out) {
    v = trim(v);
    if (v.size() < 2 || v.front() != '[' || v.back() != ']') return false;
    v = v.substr(1, v.size() - 2);
    while (!v.empty()) {
        size_t comma = v.find(',');
        std::string_view item = (comma == std::string_view::npos) ? v : v.substr(0, comma);
        std::string s;
        item = trim(item);
        if (!item.empty()) {
            if (!unquote(item, s)) return false;
            out.push_back(s);
        }
        if (comma == std::string_view::npos) break;
        v.remove_prefix(comma + 1);
    }
    return true;
}

} // namespace

Config parse_config(std::string_view text, std::string& error) {
    Config cfg;
    size_t line_no = 0, i = 0;

    while (i < text.size()) {
        size_t end = text.find('\n', i);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = trim(strip_comment(text.substr(i, end - i)));
        i = end + 1;
        ++line_no;
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            error = "line " + std::to_string(line_no) + ": expected 'key = value'";
            return cfg;
        }
        std::string_view key = trim(line.substr(0, eq));
        std::string_view val = trim(line.substr(eq + 1));

        auto bad_value = [&] {
            error = "line " + std::to_string(line_no) + ": bad value for '" +
                    std::string(key) + "'";
        };

        if (key == "default_board") {
            if (!unquote(val, cfg.default_board)) { bad_value(); return cfg; }
        } else if (key == "default_port") {
            if (!unquote(val, cfg.default_port)) { bad_value(); return cfg; }
        } else if (key == "monitor_baud") {
            cfg.monitor_baud = std::atoi(std::string(val).c_str());
            if (cfg.monitor_baud <= 0) { bad_value(); return cfg; }
        } else if (key == "search_roots") {
            if (!parse_array(val, cfg.search_roots)) { bad_value(); return cfg; }
        } else {
            error = "line " + std::to_string(line_no) + ": unknown key '" +
                    std::string(key) + "'";
            return cfg;
        }
    }
    return cfg;
}

} // namespace ardio
```

Create `src/toolchain/search.cpp`:

```cpp
#include "ardio/toolchain.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace ardio {
namespace {

std::string home() {
    const char* h = std::getenv("HOME");
    return h ? h : "";
}

// Expands a leading "~/" against $HOME.
std::string expand(std::string_view p) {
    if (p.size() >= 2 && p[0] == '~' && p[1] == '/') return home() + std::string(p.substr(1));
    return std::string(p);
}

bool is_executable(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) &&
           (fs::status(p, ec).permissions() & fs::perms::owner_exec) != fs::perms::none;
}

// Looks for `name` directly in `root`, in root/bin, and one level down
// (vendor package layouts nest as root/<tool>/<version>/bin).
bool search_root(const fs::path& root, std::string_view name, std::string& out) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return false;

    for (const fs::path& candidate : {root / name, root / "bin" / name}) {
        if (is_executable(candidate)) { out = candidate.string(); return true; }
    }

    for (const auto& entry : fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec) && entry.path().filename() == name &&
            is_executable(entry.path())) {
            out = entry.path().string();
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<std::string> default_search_roots() {
    return {
        "~/.ardio/tools",
        "/usr/local/ardio",
        "~/.arduino-create",
        "~/Library/Arduino15",
        "$PATH",
    };
}

ToolLocation find_tool(std::string_view name, const std::vector<std::string>& roots) {
    ToolLocation loc;
    for (const std::string& root : roots) {
        if (root == "$PATH") {
            const char* path_env = std::getenv("PATH");
            if (!path_env) continue;
            std::string_view rest(path_env);
            while (!rest.empty()) {
                size_t colon = rest.find(':');
                std::string_view dir = (colon == std::string_view::npos)
                                           ? rest : rest.substr(0, colon);
                fs::path candidate = fs::path(std::string(dir)) / name;
                if (is_executable(candidate)) {
                    loc.path = candidate.string();
                    loc.found_in_root = "$PATH";
                    loc.found = true;
                    return loc;
                }
                if (colon == std::string_view::npos) break;
                rest.remove_prefix(colon + 1);
            }
            continue;
        }

        std::string found;
        if (search_root(expand(root), name, found)) {
            loc.path = found;
            loc.found_in_root = root;
            loc.found = true;
            return loc;
        }
    }
    return loc;
}

} // namespace ardio
```

Modify `CMakeLists.txt`:

```cmake
target_sources(libardio PRIVATE src/hex.cpp src/board.cpp src/protocol/stk500v1.cpp
                                src/protocol/programmer.cpp src/platform/ports_select.cpp
                                src/toolchain/config.cpp src/toolchain/search.cpp)

add_executable(ardio_tests tests/main.cpp tests/test_harness.cpp tests/test_hex.cpp
                           tests/test_board.cpp tests/test_stk500v1.cpp
                           tests/test_serial_fake.cpp tests/test_programmer.cpp
                           tests/test_ports.cpp tests/test_config.cpp)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd ~/ardio && cmake --build build && ./build/ardio_tests`
Expected: PASS — 45 tests, 0 failures.

- [ ] **Step 5: Commit**

```bash
cd ~/ardio
git add include/ardio/toolchain.h src/toolchain tests/test_config.cpp CMakeLists.txt
git commit -m "feat: add config parser and toolchain search path"
```

---

### Task 9: Sketch build stage

**Files:**
- Create: `include/ardio/build.h`
- Create: `src/build/compile.cpp`
- Create: `tests/test_build.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Board` (Task 3), `find_tool` (Task 8).
- Produces:
  - `struct ardio::CompileCommand { std::string program; std::vector<std::string> args; };`
  - `CompileCommand ardio::make_compile_command(const std::string& gcc_path, const Board& board, const std::string& source, const std::string& out_elf);`
  - `CompileCommand ardio::make_objcopy_command(const std::string& objcopy_path, const std::string& in_elf, const std::string& out_hex);`
  - `struct ardio::BuildResult { bool ok; std::string error; std::string hex_path; };`
  - `BuildResult ardio::build_sketch(const std::string& source, const Board& board, const std::vector<std::string>& roots, const std::string& out_dir);`

**There is no `avr-gcc` on this machine** — `~/.arduino-create/arduino/` ships uploaders only. So the command *construction* is a pure function and fully unit-tested here, while `build_sketch` is written to fail with an actionable message naming every root it searched. End-to-end compilation is verified in Task 11's manual step once a toolchain is present.

- [ ] **Step 1: Write the failing test**

Create `tests/test_build.cpp`:

```cpp
#include "harness.h"
#include "ardio/build.h"
#include "ardio/board.h"
#include <algorithm>
#include <string>

namespace {
bool has_arg(const ardio::CompileCommand& c, const std::string& want) {
    return std::find(c.args.begin(), c.args.end(), want) != c.args.end();
}
} // namespace

TEST(compile_command_sets_mcu_and_fcpu_from_board) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    auto cmd = ardio::make_compile_command("/opt/avr/bin/avr-g++", *nano,
                                           "blink.cpp", "blink.elf");
    CHECK(cmd.program == "/opt/avr/bin/avr-g++");
    CHECK(has_arg(cmd, "-mmcu=atmega328p"));
    CHECK(has_arg(cmd, "-DF_CPU=16000000L"));
}

TEST(compile_command_optimises_for_size_and_names_output) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    auto cmd = ardio::make_compile_command("/opt/avr/bin/avr-g++", *nano,
                                           "blink.cpp", "blink.elf");
    CHECK(has_arg(cmd, "-Os"));           // AVR flash is 32KB; size beats speed
    CHECK(has_arg(cmd, "blink.cpp"));
    CHECK(has_arg(cmd, "-o"));
    CHECK(has_arg(cmd, "blink.elf"));
}

TEST(objcopy_command_produces_ihex) {
    auto cmd = ardio::make_objcopy_command("/opt/avr/bin/avr-objcopy",
                                           "blink.elf", "blink.hex");
    CHECK(cmd.program == "/opt/avr/bin/avr-objcopy");
    CHECK(has_arg(cmd, "-O"));
    CHECK(has_arg(cmd, "ihex"));
    CHECK(has_arg(cmd, "-R"));
    CHECK(has_arg(cmd, ".eeprom"));       // EEPROM section is not flash content
    CHECK(has_arg(cmd, "blink.elf"));
    CHECK(has_arg(cmd, "blink.hex"));
}

TEST(build_reports_missing_toolchain_with_every_root_searched) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    auto result = ardio::build_sketch("blink.cpp", *nano,
                                      {"/nonexistent-a", "/nonexistent-b"}, "/tmp");
    CHECK(!result.ok);
    CHECK(result.error.find("avr-g++") != std::string::npos);
    CHECK(result.error.find("/nonexistent-a") != std::string::npos);
    CHECK(result.error.find("/nonexistent-b") != std::string::npos);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd ~/ardio && cmake --build build 2>&1 | head -5`
Expected: FAIL — `'ardio/build.h' file not found`.

- [ ] **Step 3: Write minimal implementation**

Create `include/ardio/build.h`:

```cpp
#pragma once
#include "ardio/board.h"
#include <string>
#include <vector>

namespace ardio {

struct CompileCommand {
    std::string program;
    std::vector<std::string> args;
};

CompileCommand make_compile_command(const std::string& gcc_path, const Board& board,
                                    const std::string& source, const std::string& out_elf);

CompileCommand make_objcopy_command(const std::string& objcopy_path,
                                    const std::string& in_elf, const std::string& out_hex);

struct BuildResult {
    bool ok = false;
    std::string error;
    std::string hex_path;
};

// Locates avr-g++/avr-objcopy via `roots`, compiles `source`, and emits a
// .hex into `out_dir`. Fails with an actionable message if the toolchain is
// not installed.
BuildResult build_sketch(const std::string& source, const Board& board,
                         const std::vector<std::string>& roots,
                         const std::string& out_dir);

} // namespace ardio
```

Create `src/build/compile.cpp`:

```cpp
#include "ardio/build.h"
#include "ardio/toolchain.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace ardio {
namespace {

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    out += "'";
    return out;
}

// Runs a command, returning its exit status. Output goes to the terminal so
// compiler diagnostics reach the user unfiltered.
int run(const CompileCommand& cmd) {
    std::string line = shell_quote(cmd.program);
    for (const std::string& a : cmd.args) line += " " + shell_quote(a);
    return std::system(line.c_str());
}

std::string roots_list(const std::vector<std::string>& roots) {
    std::string s;
    for (const std::string& r : roots) s += "\n  " + r;
    return s;
}

} // namespace

CompileCommand make_compile_command(const std::string& gcc_path, const Board& board,
                                    const std::string& source,
                                    const std::string& out_elf) {
    CompileCommand cmd;
    cmd.program = gcc_path;
    cmd.args = {
        "-mmcu=" + board.gcc_mcu,
        "-DF_CPU=" + std::to_string(board.f_cpu) + "L",
        "-Os",                    // 32KB of flash: optimise for size
        "-std=c++20",
        "-ffunction-sections",    // with --gc-sections, drops unused code
        "-fdata-sections",
        "-Wl,--gc-sections",
        source,
        "-o",
        out_elf,
    };
    return cmd;
}

CompileCommand make_objcopy_command(const std::string& objcopy_path,
                                    const std::string& in_elf,
                                    const std::string& out_hex) {
    CompileCommand cmd;
    cmd.program = objcopy_path;
    cmd.args = {"-O", "ihex", "-R", ".eeprom", in_elf, out_hex};
    return cmd;
}

BuildResult build_sketch(const std::string& source, const Board& board,
                         const std::vector<std::string>& roots,
                         const std::string& out_dir) {
    BuildResult result;

    ToolLocation gcc = find_tool("avr-g++", roots);
    if (!gcc.found) {
        result.error = "avr-g++ not found. Searched:" + roots_list(roots) +
                       "\nRun 'ardio toolchain fetch avr' to install it.";
        return result;
    }
    ToolLocation objcopy = find_tool("avr-objcopy", roots);
    if (!objcopy.found) {
        result.error = "avr-objcopy not found. Searched:" + roots_list(roots) +
                       "\nRun 'ardio toolchain fetch avr' to install it.";
        return result;
    }

    std::error_code ec;
    fs::create_directories(out_dir, ec);

    std::string stem = fs::path(source).stem().string();
    std::string elf = (fs::path(out_dir) / (stem + ".elf")).string();
    std::string hex = (fs::path(out_dir) / (stem + ".hex")).string();

    if (run(make_compile_command(gcc.path, board, source, elf)) != 0) {
        result.error = "compilation failed";
        return result;
    }
    if (run(make_objcopy_command(objcopy.path, elf, hex)) != 0) {
        result.error = "avr-objcopy failed to produce " + hex;
        return result;
    }

    result.ok = true;
    result.hex_path = hex;
    return result;
}

} // namespace ardio
```

Modify `CMakeLists.txt`:

```cmake
target_sources(libardio PRIVATE src/hex.cpp src/board.cpp src/protocol/stk500v1.cpp
                                src/protocol/programmer.cpp src/platform/ports_select.cpp
                                src/toolchain/config.cpp src/toolchain/search.cpp
                                src/build/compile.cpp)

add_executable(ardio_tests tests/main.cpp tests/test_harness.cpp tests/test_hex.cpp
                           tests/test_board.cpp tests/test_stk500v1.cpp
                           tests/test_serial_fake.cpp tests/test_programmer.cpp
                           tests/test_ports.cpp tests/test_config.cpp tests/test_build.cpp)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd ~/ardio && cmake --build build && ./build/ardio_tests`
Expected: PASS — 49 tests, 0 failures.

- [ ] **Step 5: Commit**

```bash
cd ~/ardio
git add include/ardio/build.h src/build tests/test_build.cpp CMakeLists.txt
git commit -m "feat: add sketch build stage with avr-gcc command construction"
```

---

### Task 10: Serial monitor

**Files:**
- Create: `include/ardio/monitor.h`
- Create: `src/monitor.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SerialPort` (Task 5).
- Produces: `void ardio::run_monitor(SerialPort& port, volatile bool& stop_flag);`

The monitor is a read-print loop over the `SerialPort` interface, so it works against any implementation. It has no unit test of its own — the loop is three lines and everything it depends on is already covered — but it is exercised in Task 11's manual verification.

- [ ] **Step 1: Write the implementation**

Create `include/ardio/monitor.h`:

```cpp
#pragma once
#include "ardio/platform/serial.h"

namespace ardio {

// Reads from `port` and writes to stdout until `stop_flag` becomes true.
void run_monitor(SerialPort& port, volatile bool& stop_flag);

} // namespace ardio
```

Create `src/monitor.cpp`:

```cpp
#include "ardio/monitor.h"
#include <cstdio>

namespace ardio {

void run_monitor(SerialPort& port, volatile bool& stop_flag) {
    uint8_t buf[512];
    while (!stop_flag) {
        size_t n = port.read(buf, sizeof(buf), 100);
        if (n > 0) {
            std::fwrite(buf, 1, n, stdout);
            std::fflush(stdout);
        }
    }
}

} // namespace ardio
```

Modify `CMakeLists.txt` — add `src/monitor.cpp` to the `target_sources` list for `libardio`.

- [ ] **Step 2: Verify it compiles**

Run: `cd ~/ardio && cmake --build build && ./build/ardio_tests`
Expected: PASS — 49 tests, 0 failures (no new tests; the build must stay clean under `-Werror`).

- [ ] **Step 3: Commit**

```bash
cd ~/ardio
git add include/ardio/monitor.h src/monitor.cpp CMakeLists.txt
git commit -m "feat: add serial monitor loop"
```

---

### Task 11: CLI

**Files:**
- Create: `include/ardio/cli.h`
- Create: `src/cli/args.cpp` (argument parsing — in `libardio`, so tests can link it)
- Create: `src/cli/commands.cpp` (command dispatch — in `libardio`)
- Create: `src/cli/main.cpp` (`main()` only — the executable)
- Create: `tests/test_cli_args.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Tasks 2–10.
- Produces:
  - `struct ardio::Args { std::string command; std::string positional; std::string port; std::string board; int baud; bool monitor_after; bool help; std::string error; };`
  - `Args ardio::parse_args(int argc, char** argv);`
  - `int ardio::run_command(const Args& args);`
  - The `ardio` executable target.

Argument parsing is split from command execution so the flag handling is unit-testable while the commands themselves stay thin. This task delivers the whole Phase 1 command surface: `ports`, `boards`, `doctor`, `build`, `flash`, `push`, `monitor`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_cli_args.cpp`:

```cpp
#include "harness.h"
#include "ardio/cli.h"
#include <string>
#include <vector>

namespace {
ardio::Args parse(std::vector<const char*> argv) {
    return ardio::parse_args(int(argv.size()), const_cast<char**>(argv.data()));
}
} // namespace

TEST(cli_parses_bare_command) {
    auto a = parse({"ardio", "ports"});
    CHECK(a.command == "ports");
    CHECK(a.error.empty());
}

TEST(cli_parses_push_with_sketch_and_port) {
    auto a = parse({"ardio", "push", "blink.cpp", "--port", "/dev/cu.usbserial-110"});
    CHECK(a.command == "push");
    CHECK(a.positional == "blink.cpp");
    CHECK(a.port == "/dev/cu.usbserial-110");
}

TEST(cli_parses_board_and_baud) {
    auto a = parse({"ardio", "monitor", "--board", "nano", "--baud", "115200"});
    CHECK(a.board == "nano");
    CHECK_EQ(a.baud, 115200);
}

TEST(cli_parses_monitor_after_flag) {
    auto a = parse({"ardio", "push", "blink.cpp", "-m"});
    CHECK(a.monitor_after);
}

TEST(cli_errors_on_flag_missing_its_value) {
    auto a = parse({"ardio", "push", "--port"});
    CHECK(!a.error.empty());
    CHECK(a.error.find("--port") != std::string::npos);
}

TEST(cli_errors_on_unknown_flag) {
    auto a = parse({"ardio", "push", "--nonsense"});
    CHECK(a.error.find("--nonsense") != std::string::npos);
}

TEST(cli_no_command_requests_help) {
    auto a = parse({"ardio"});
    CHECK(a.help);
}

TEST(cli_help_flag_sets_help) {
    auto a = parse({"ardio", "--help"});
    CHECK(a.help);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd ~/ardio && cmake --build build 2>&1 | head -5`
Expected: FAIL — `'ardio/cli.h' file not found`.

- [ ] **Step 3: Write minimal implementation**

Create `include/ardio/cli.h`:

```cpp
#pragma once
#include <string>

namespace ardio {

struct Args {
    std::string command;     // "ports", "push", ...
    std::string positional;  // sketch or hex path
    std::string port;
    std::string board;
    int baud = 0;            // 0 = use config/board default
    bool monitor_after = false;
    bool help = false;
    std::string error;
};

Args parse_args(int argc, char** argv);
int run_command(const Args& args);

} // namespace ardio
```

Create `src/cli/commands.cpp`:

```cpp
#include "ardio/cli.h"
#include "ardio/board.h"
#include "ardio/build.h"
#include "ardio/hex.h"
#include "ardio/monitor.h"
#include "ardio/platform/ports.h"
#include "ardio/protocol/programmer.h"
#include "ardio/toolchain.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ardio {
namespace {

volatile bool g_stop = false;
void on_sigint(int) { g_stop = true; }

std::string usb_id_string(const PortInfo& p) {
    if (!p.has_usb_id) return "-";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04x:%04x", p.usb.vid, p.usb.pid);
    return buf;
}

Config load_config() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    fs::path path = fs::path(home) / ".ardio" / "config.toml";
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    std::string error;
    Config cfg = parse_config(ss.str(), error);
    if (!error.empty())
        std::fprintf(stderr, "warning: %s: %s\n", path.c_str(), error.c_str());
    return cfg;
}

std::vector<std::string> roots_for(const Config& cfg) {
    return cfg.search_roots.empty() ? default_search_roots() : cfg.search_roots;
}

int cmd_ports() {
    auto ports = enumerate_ports();
    if (ports.empty()) {
        std::printf("no serial ports found\n");
        return 1;
    }
    for (const PortInfo& p : ports) {
        auto boards = p.has_usb_id ? find_boards_by_usb(p.usb) : std::vector<const Board*>{};
        std::printf("%-28s %-12s %s\n", p.device.c_str(), usb_id_string(p).c_str(),
                    boards.empty() ? "(unrecognised)" : boards[0]->name.c_str());
    }
    return 0;
}

int cmd_boards() {
    for (const Board& b : board_database())
        std::printf("%-10s %-32s flash %uKB  page %u\n", b.id.c_str(), b.name.c_str(),
                    b.flash_size / 1024, b.page_size);
    return 0;
}

int cmd_doctor() {
    Config cfg = load_config();
    auto roots = roots_for(cfg);

    std::printf("search roots:\n");
    for (const std::string& r : roots) std::printf("  %s\n", r.c_str());

    std::printf("\ntools:\n");
    int missing = 0;
    for (const char* tool : {"avr-g++", "avr-objcopy"}) {
        ToolLocation loc = find_tool(tool, roots);
        if (loc.found)
            std::printf("  %-14s %s  (via %s)\n", tool, loc.path.c_str(),
                        loc.found_in_root.c_str());
        else {
            std::printf("  %-14s NOT FOUND\n", tool);
            ++missing;
        }
    }

    std::printf("\nports:\n");
    auto ports = enumerate_ports();
    if (ports.empty()) std::printf("  none\n");
    for (const PortInfo& p : ports)
        std::printf("  %-28s %s\n", p.device.c_str(), usb_id_string(p).c_str());

    if (missing)
        std::printf("\n%d tool(s) missing. ardio can flash prebuilt .hex files, "
                    "but cannot compile sketches until an AVR toolchain is installed.\n",
                    missing);
    return missing == 0 ? 0 : 1;
}

// Resolves port + board, printing the reason on failure.
bool resolve(const Args& args, const Config& cfg, PortInfo& out_port, const Board*& out_board) {
    auto ports = enumerate_ports();
    std::string want_port = args.port.empty() ? cfg.default_port : args.port;
    std::string want_board = args.board.empty() ? cfg.default_board : args.board;

    PortSelection sel = select_port(ports, want_port, want_board);
    if (!sel.port || !sel.board) {
        std::fprintf(stderr, "error: %s\n", sel.error.c_str());
        return false;
    }
    out_port = *sel.port;
    out_board = sel.board;
    return true;
}

int do_flash(const std::string& hex_path, const PortInfo& port, const Board& board) {
    std::ifstream in(hex_path);
    if (!in) {
        std::fprintf(stderr, "error: cannot read %s\n", hex_path.c_str());
        return 1;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    std::string error;
    auto image = parse_intel_hex(ss.str(), error);
    if (!image) {
        std::fprintf(stderr, "error: %s: %s\n", hex_path.c_str(), error.c_str());
        return 1;
    }

    auto serial = make_serial_port();
    auto result = upload_stk500v1(*serial, port.device, board, *image,
                                  [](const std::string& msg) {
                                      std::printf("  %s\n", msg.c_str());
                                  });
    if (!result.ok) {
        std::fprintf(stderr, "error [%s]: %s\n", result.stage.c_str(), result.error.c_str());
        return 1;
    }
    std::printf("uploaded %zu bytes to %s at %d baud\n", image->data.size(),
                port.device.c_str(), result.baud_used);
    return 0;
}

int do_monitor(const PortInfo& port, int baud) {
    auto serial = make_serial_port();
    std::string error;
    if (!serial->open(port.device, baud, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("--- monitoring %s at %d baud (ctrl-c to stop) ---\n",
                port.device.c_str(), baud);
    g_stop = false;
    std::signal(SIGINT, on_sigint);
    run_monitor(*serial, g_stop);
    std::printf("\n");
    return 0;
}

} // namespace

int run_command(const Args& args) {
    if (args.help || args.command.empty()) {
        std::printf(
            "ardio -- build, flash, and monitor Arduino boards\n\n"
            "  ardio push [sketch]      build and upload\n"
            "  ardio build [sketch]     compile only\n"
            "  ardio flash <file.hex>   upload a prebuilt image\n"
            "  ardio monitor            open the serial monitor\n"
            "  ardio ports              list serial ports\n"
            "  ardio boards             list supported boards\n"
            "  ardio doctor             diagnose toolchains and ports\n\n"
            "  --port <dev>   --board <id>   --baud <n>   -m (monitor after push)\n");
        return 0;
    }
    if (!args.error.empty()) {
        std::fprintf(stderr, "error: %s\n", args.error.c_str());
        return 2;
    }

    if (args.command == "ports")  return cmd_ports();
    if (args.command == "boards") return cmd_boards();
    if (args.command == "doctor") return cmd_doctor();

    Config cfg = load_config();

    if (args.command == "build" || args.command == "push") {
        if (args.positional.empty()) {
            std::fprintf(stderr, "error: %s needs a sketch file\n", args.command.c_str());
            return 2;
        }
        const Board* board = args.board.empty() ? find_board_by_id("nano")
                                                : find_board_by_id(args.board);
        PortInfo port;
        if (args.command == "push" && !resolve(args, cfg, port, board)) return 1;
        if (!board) {
            std::fprintf(stderr, "error: unknown board '%s'\n", args.board.c_str());
            return 2;
        }

        auto result = build_sketch(args.positional, *board, roots_for(cfg), "build/ardio");
        if (!result.ok) {
            std::fprintf(stderr, "error: %s\n", result.error.c_str());
            return 1;
        }
        std::printf("built %s\n", result.hex_path.c_str());
        if (args.command == "build") return 0;

        int rc = do_flash(result.hex_path, port, *board);
        if (rc != 0) return rc;
        if (args.monitor_after)
            return do_monitor(port, args.baud ? args.baud : cfg.monitor_baud);
        return 0;
    }

    if (args.command == "flash") {
        if (args.positional.empty()) {
            std::fprintf(stderr, "error: flash needs a .hex file\n");
            return 2;
        }
        PortInfo port; const Board* board = nullptr;
        if (!resolve(args, cfg, port, board)) return 1;
        return do_flash(args.positional, port, *board);
    }

    if (args.command == "monitor") {
        PortInfo port; const Board* board = nullptr;
        if (!resolve(args, cfg, port, board)) return 1;
        return do_monitor(port, args.baud ? args.baud : cfg.monitor_baud);
    }

    std::fprintf(stderr, "error: unknown command '%s'. Try 'ardio --help'.\n",
                 args.command.c_str());
    return 2;
}

} // namespace ardio
```

Create `src/cli/args.cpp` — argument parsing lives in `libardio` so the tests can link it. Only `main()` stays in the executable:

```cpp
#include "ardio/cli.h"
#include <cstdlib>
#include <string>

namespace ardio {

Args parse_args(int argc, char** argv) {
    Args a;
    if (argc < 2) { a.help = true; return a; }

    int i = 1;
    std::string first = argv[1];
    if (first == "--help" || first == "-h") { a.help = true; return a; }
    a.command = first;
    ++i;

    // A value-taking flag consumes the next argv entry.
    auto take_value = [&](const char* flag, std::string& dest) -> bool {
        if (i + 1 >= argc) {
            a.error = std::string("flag ") + flag + " needs a value";
            return false;
        }
        dest = argv[++i];
        return true;
    };

    for (; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port")       { if (!take_value("--port", a.port)) return a; }
        else if (arg == "--board") { if (!take_value("--board", a.board)) return a; }
        else if (arg == "--baud") {
            std::string v;
            if (!take_value("--baud", v)) return a;
            a.baud = std::atoi(v.c_str());
            if (a.baud <= 0) { a.error = "--baud needs a positive number"; return a; }
        }
        else if (arg == "-m" || arg == "--monitor") { a.monitor_after = true; }
        else if (arg == "--help" || arg == "-h")    { a.help = true; }
        else if (!arg.empty() && arg[0] == '-') {
            a.error = "unknown flag '" + arg + "'";
            return a;
        }
        else if (a.positional.empty()) { a.positional = arg; }
        else { a.error = "unexpected argument '" + arg + "'"; return a; }
    }
    return a;
}

} // namespace ardio
```

Create `src/cli/main.cpp` — the entire executable:

```cpp
#include "ardio/cli.h"

int main(int argc, char** argv) {
    return ardio::run_command(ardio::parse_args(argc, argv));
}
```

Modify `CMakeLists.txt`:

```cmake
target_sources(libardio PRIVATE src/hex.cpp src/board.cpp src/protocol/stk500v1.cpp
                                src/protocol/programmer.cpp src/platform/ports_select.cpp
                                src/toolchain/config.cpp src/toolchain/search.cpp
                                src/build/compile.cpp src/monitor.cpp
                                src/cli/args.cpp src/cli/commands.cpp)

add_executable(ardio src/cli/main.cpp)
target_link_libraries(ardio PRIVATE libardio)

add_executable(ardio_tests tests/main.cpp tests/test_harness.cpp tests/test_hex.cpp
                           tests/test_board.cpp tests/test_stk500v1.cpp
                           tests/test_serial_fake.cpp tests/test_programmer.cpp
                           tests/test_ports.cpp tests/test_config.cpp tests/test_build.cpp
                           tests/test_cli_args.cpp)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd ~/ardio && cmake --build build && ./build/ardio_tests`
Expected: PASS — 57 tests, 0 failures.

- [ ] **Step 5: Verify against real hardware**

With the Nano plugged in:

```bash
cd ~/ardio
./build/ardio ports
```
Expected: lists `/dev/cu.usbserial-110` with `1a86:7523` and `Arduino Nano (ATmega328P)`.

```bash
./build/ardio doctor
```
Expected: prints the search roots, reports `avr-g++ NOT FOUND` (no AVR toolchain is installed yet), and lists the port.

```bash
./build/ardio monitor --baud 9600
```
Expected: opens the port and prints whatever the currently-flashed sketch emits. Ctrl-C exits cleanly.

If an AVR toolchain is available, also confirm the full path with a blink sketch:

```bash
./build/ardio push blink.cpp -m
```
Expected: compiles, syncs (reporting which baud worked), writes pages with progress, and drops into the monitor.

- [ ] **Step 6: Commit**

```bash
cd ~/ardio
git add include/ardio/cli.h src/cli tests/test_cli_args.cpp CMakeLists.txt
git commit -m "feat: add ardio CLI with ports, boards, doctor, build, flash, push, monitor"
```

---

## Phase 1 Complete

At this point `ardio` finds a board, identifies it by USB ID, compiles a sketch when a toolchain is present, flashes it over a from-scratch STK500v1 implementation with automatic baud fallback, and monitors the result — with every protocol byte and every auto-detection branch under test without hardware.

Deferred to later phases, per the spec: the ATmega328P emulator (Phase 2), virtual peripherals (Phase 3), other board families and the Linux/Windows platform layers (Phase 4), and `ardio toolchain fetch`, which Task 9's error message promises. If a toolchain fetch is wanted before Phase 4, it is a small standalone addition to `src/toolchain/`.
