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

int run(const std::string& cmd) { return std::system(cmd.c_str()); }

// Reads the first whitespace-delimited token of a command's stdout.
std::string capture_word(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    char buf[256] = {0};
    if (std::fscanf(p, "%255s", buf) != 1) buf[0] = '\0';
    pclose(p);
    return buf;
}

bool have(const char* tool) {
    return run("command -v " + std::string(tool) + " >/dev/null 2>&1") == 0;
}

} // namespace

const std::vector<RemoteTool>& remote_tool_registry() {
    // Official Arduino toolchain builds, pinned by SHA-256. Sizes are in bytes.
    static const std::vector<RemoteTool> reg = {
        {"avr", "x86_64-apple-darwin14", "7.3.0-atmel3.6.1-arduino7",
         "https://downloads.arduino.cc/tools/"
         "avr-gcc-7.3.0-atmel3.6.1-arduino7-x86_64-apple-darwin14.tar.bz2",
         "f6ed2346953fcf88df223469088633eb86de997fa27ece117fd1ef170d69c1f8",
         36684546, "avr"},
        {"avr", "x86_64-linux-gnu", "7.3.0-atmel3.6.1-arduino7",
         "https://downloads.arduino.cc/tools/"
         "avr-gcc-7.3.0-atmel3.6.1-arduino7-x86_64-pc-linux-gnu.tar.bz2",
         "bd8c37f6952a2130ac9ee32c53f6a660feb79bee8353c8e289eb60fdcefed91e",
         37804616, "avr"},
        {"avr", "aarch64-linux-gnu", "7.3.0-atmel3.6.1-arduino7",
         "https://downloads.arduino.cc/tools/"
         "avr-gcc-7.3.0-atmel3.6.1-arduino7-aarch64-pc-linux-gnu.tar.bz2",
         "03d322b9df6da17289e9e7c6233c34a8535d9c645c19efc772ba19e56914f339",
         34702520, "avr"},
        {"avr", "arm-linux-gnueabihf", "7.3.0-atmel3.6.1-arduino7",
         "https://downloads.arduino.cc/tools/"
         "avr-gcc-7.3.0-atmel3.6.1-arduino7-arm-linux-gnueabihf.tar.bz2",
         "3903553d035da59e33cff9941b857c3cb379cb0638105dfdf69c97f0acc8e7b5",
         33191474, "avr"},
        {"avr", "i686-linux-gnu", "7.3.0-atmel3.6.1-arduino7",
         "https://downloads.arduino.cc/tools/"
         "avr-gcc-7.3.0-atmel3.6.1-arduino7-i686-pc-linux-gnu.tar.bz2",
         "954bbffb33545bcdcd473af993da2980bf32e8461ff55a18e0eebc7b2ef69a4c",
         38065465, "avr"},
        {"avr", "i686-mingw32", "7.3.0-atmel3.6.1-arduino7",
         "https://downloads.arduino.cc/tools/"
         "avr-gcc-7.3.0-atmel3.6.1-arduino7-i686-w64-mingw32.zip",
         "a54f64755fff4cb792a1495e5defdd789902a2a3503982e81b898299cf39800e",
         48787637, "avr"},
    };
    return reg;
}

std::string host_triple_for(std::string_view os, std::string_view machine) {
    if (os == "Darwin") {
        // No arm64 macOS build is published; Apple Silicon runs the x86_64
        // build under Rosetta 2, which is fine for a compiler.
        if (machine == "arm64" || machine == "aarch64" || machine == "x86_64")
            return "x86_64-apple-darwin14";
        return {};
    }
    if (os == "Linux") {
        if (machine == "x86_64")  return "x86_64-linux-gnu";
        if (machine == "aarch64" || machine == "arm64") return "aarch64-linux-gnu";
        if (machine == "armv7l" || machine == "armv6l") return "arm-linux-gnueabihf";
        if (machine == "i686" || machine == "i386")     return "i686-linux-gnu";
        return {};
    }
    return {};
}

std::string host_triple() {
    std::string os = capture_word("uname -s");
    std::string machine = capture_word("uname -m");
    return host_triple_for(os, machine);
}

const RemoteTool* find_remote_tool(std::string_view package, std::string_view host) {
    for (const RemoteTool& t : remote_tool_registry())
        if (t.package == package && t.host == host) return &t;
    return nullptr;
}

FetchResult fetch_remote_tool(const RemoteTool& tool, const std::string& dest_root,
                              const std::function<void(const std::string&)>& progress) {
    FetchResult result;
    auto report = [&](const std::string& m) { if (progress) progress(m); };

    if (!have("curl")) {
        result.error = "curl is required to download toolchains but was not found";
        return result;
    }
    if (!have("tar")) {
        result.error = "tar is required to unpack toolchains but was not found";
        return result;
    }

    std::error_code ec;
    fs::path root(dest_root);
    fs::path staging = root / ".staging";
    fs::create_directories(staging, ec);
    if (ec) {
        result.error = "cannot create " + staging.string() + ": " + ec.message();
        return result;
    }

    fs::path archive = staging / fs::path(tool.url).filename();

    report("downloading " + tool.url);
    // --fail turns HTTP errors into a non-zero exit; -L follows redirects.
    if (run("curl --fail --location --progress-bar -o " + shell_quote(archive.string()) +
            " " + shell_quote(tool.url)) != 0) {
        result.error = "download failed: " + tool.url;
        fs::remove_all(staging, ec);
        return result;
    }

    report("verifying checksum");
    std::string sum_cmd = have("shasum")
                              ? "shasum -a 256 " + shell_quote(archive.string())
                              : "sha256sum " + shell_quote(archive.string());
    std::string got = capture_word(sum_cmd);
    if (got.empty()) {
        result.error = "could not compute the archive checksum";
        fs::remove_all(staging, ec);
        return result;
    }
    if (got != tool.sha256) {
        result.error = "checksum mismatch -- refusing to install.\n  expected " +
                       tool.sha256 + "\n  got      " + got;
        fs::remove_all(staging, ec);
        return result;
    }

    report("unpacking");
    fs::path unpack = staging / "unpacked";
    fs::create_directories(unpack, ec);
    bool is_zip = archive.extension() == ".zip";
    std::string extract = is_zip
        ? "unzip -q " + shell_quote(archive.string()) + " -d " + shell_quote(unpack.string())
        : "tar -xf " + shell_quote(archive.string()) + " -C " + shell_quote(unpack.string());
    if (run(extract) != 0) {
        result.error = "failed to unpack " + archive.filename().string();
        fs::remove_all(staging, ec);
        return result;
    }

    // The archive holds a single top-level directory; move it into place.
    fs::path produced;
    for (const auto& e : fs::directory_iterator(unpack, ec)) {
        if (e.is_directory(ec)) { produced = e.path(); break; }
    }
    if (produced.empty()) {
        result.error = "archive did not contain a toolchain directory";
        fs::remove_all(staging, ec);
        return result;
    }

    fs::path final_dir = root / (tool.package + "-" + tool.version);
    fs::remove_all(final_dir, ec);
    fs::rename(produced, final_dir, ec);
    if (ec) {
        result.error = "cannot move toolchain into " + final_dir.string() + ": " +
                       ec.message();
        fs::remove_all(staging, ec);
        return result;
    }

    fs::remove_all(staging, ec);
    result.ok = true;
    result.installed_to = final_dir.string();
    return result;
}

} // namespace ardio
