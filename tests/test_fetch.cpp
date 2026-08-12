#include "harness.h"
#include "ardio/toolchain.h"
#include <string>

TEST(host_triple_is_detected_for_this_machine) {
    // Whatever machine runs the suite, detection must produce a non-empty
    // triple that the registry can be queried with.
    std::string host = ardio::host_triple();
    CHECK(!host.empty());
}

TEST(apple_silicon_maps_to_the_x86_64_darwin_build) {
    // Arduino ships no arm64 macOS avr-gcc; Apple Silicon runs the x86_64
    // build under Rosetta 2. Mapping arm64 -> x86_64 is deliberate.
    CHECK(ardio::host_triple_for("Darwin", "arm64") == "x86_64-apple-darwin14");
    CHECK(ardio::host_triple_for("Darwin", "x86_64") == "x86_64-apple-darwin14");
}

TEST(linux_hosts_map_to_their_own_builds) {
    CHECK(ardio::host_triple_for("Linux", "x86_64") == "x86_64-linux-gnu");
    CHECK(ardio::host_triple_for("Linux", "aarch64") == "aarch64-linux-gnu");
}

TEST(unknown_host_maps_to_empty_triple) {
    CHECK(ardio::host_triple_for("Plan9", "sparc").empty());
}

TEST(registry_has_avr_gcc_for_darwin_with_url_and_checksum) {
    const ardio::RemoteTool* t = ardio::find_remote_tool("avr", "x86_64-apple-darwin14");
    CHECK(t != nullptr);
    CHECK(t->url.find("avr-gcc-7.3.0") != std::string::npos);
    CHECK(t->url.find(".tar.bz2") != std::string::npos);
    CHECK_EQ(t->sha256.size(), size_t(64));
    CHECK(t->size_bytes > 0);
}

TEST(registry_has_avr_gcc_for_linux) {
    const ardio::RemoteTool* t = ardio::find_remote_tool("avr", "x86_64-linux-gnu");
    CHECK(t != nullptr);
    CHECK(t->url.find("linux-gnu") != std::string::npos);
    CHECK_EQ(t->sha256.size(), size_t(64));
}

TEST(registry_returns_null_for_unknown_package_or_host) {
    CHECK(ardio::find_remote_tool("teapot", "x86_64-apple-darwin14") == nullptr);
    CHECK(ardio::find_remote_tool("avr", "plan9-sparc") == nullptr);
}

TEST(every_registry_entry_has_a_well_formed_https_url_and_checksum) {
    const auto& all = ardio::remote_tool_registry();
    CHECK(all.size() > 0);
    for (const ardio::RemoteTool& t : all) {
        CHECK(t.url.rfind("https://", 0) == 0);   // never plain http
        CHECK_EQ(t.sha256.size(), size_t(64));
        CHECK(t.size_bytes > 0);
        CHECK(!t.package.empty());
        CHECK(!t.host.empty());
    }
}
