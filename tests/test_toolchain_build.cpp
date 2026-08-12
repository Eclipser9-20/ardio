#include "harness.h"
#include "ardio/toolchain.h"
#include <algorithm>
#include <string>

namespace {
bool has(const std::vector<std::string>& v, const std::string& want) {
    return std::find(v.begin(), v.end(), want) != v.end();
}
bool any_contains(const std::vector<std::string>& v, const std::string& frag) {
    for (const auto& s : v)
        if (s.find(frag) != std::string::npos) return true;
    return false;
}
} // namespace

TEST(source_registry_lists_the_three_stages_in_build_order) {
    const auto& srcs = ardio::toolchain_sources();
    CHECK_EQ(srcs.size(), size_t(3));
    // binutils must precede gcc (gcc needs the assembler), and avr-libc last
    // (it is compiled *by* the avr-gcc built in stage 2).
    CHECK(srcs[0].name == "binutils");
    CHECK(srcs[1].name == "gcc");
    CHECK(srcs[2].name == "avr-libc");
}

TEST(every_source_is_https_and_checksum_pinned) {
    for (const ardio::SourcePackage& s : ardio::toolchain_sources()) {
        CHECK(s.url.rfind("https://", 0) == 0);
        CHECK_EQ(s.sha256.size(), size_t(64));
        CHECK(!s.version.empty());
    }
}

TEST(binutils_is_configured_as_an_avr_cross_assembler) {
    auto args = ardio::binutils_configure_args("/opt/tc");
    CHECK(has(args, "--target=avr"));
    CHECK(has(args, "--prefix=/opt/tc"));
    CHECK(has(args, "--disable-nls"));
}

TEST(gcc_is_configured_for_avr_with_c_and_cxx) {
    auto args = ardio::gcc_configure_args("/opt/tc");
    CHECK(has(args, "--target=avr"));
    CHECK(has(args, "--prefix=/opt/tc"));
    CHECK(has(args, "--enable-languages=c,c++"));
    // A freestanding AVR cross-compiler must not try to build host-style
    // shared libs, threads, or libssp.
    CHECK(has(args, "--disable-shared"));
    CHECK(has(args, "--disable-threads"));
    CHECK(has(args, "--disable-libssp"));
}

TEST(avr_libc_is_configured_as_host_avr) {
    auto args = ardio::avr_libc_configure_args("/opt/tc");
    CHECK(has(args, "--prefix=/opt/tc"));
    CHECK(has(args, "--host=avr"));
}

TEST(gcc_configure_does_not_hardcode_a_build_machine) {
    // --build must be detected at run time, never baked into the binary.
    auto args = ardio::gcc_configure_args("/opt/tc");
    CHECK(!any_contains(args, "--build="));
    CHECK(!any_contains(args, "apple-darwin"));
}
