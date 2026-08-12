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
