// Builds a complete AVR cross-toolchain from upstream source.
//
// ardio never vendors compiler source: the tarballs are fetched at build time,
// verified against pinned checksums, compiled, and installed under the user's
// tool directory. Nothing from GCC or binutils enters this repository.
//
// Build order matters. binutils supplies the AVR assembler and linker that GCC
// needs; GCC then produces the avr-gcc that compiles avr-libc.

#include "ardio/toolchain.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace ardio {
namespace {

std::string quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    out += "'";
    return out;
}

std::string join(const std::vector<std::string>& args) {
    std::string s;
    for (const std::string& a : args) s += " " + quote(a);
    return s;
}

// Runs a shell command in `dir`, streaming its output to the terminal so a
// long compile shows progress.
int run_in(const fs::path& dir, const std::string& cmd) {
    return std::system(("cd " + quote(dir.string()) + " && " + cmd).c_str());
}

bool have(const char* tool) {
    return std::system(("command -v " + std::string(tool) + " >/dev/null 2>&1").c_str()) == 0;
}

std::string archive_name(const std::string& url) {
    return fs::path(url).filename().string();
}

// Downloads `pkg` into `dir` and verifies its checksum. Skips the download if
// a verified copy is already present, so an interrupted build can resume.
bool fetch_source(const SourcePackage& pkg, const fs::path& dir, std::string& error,
                  const std::function<void(const std::string&)>& report) {
    fs::path archive = dir / archive_name(pkg.url);

    auto checksum_matches = [&]() {
        if (!fs::exists(archive)) return false;
        std::string cmd = have("shasum") ? "shasum -a 256 " + quote(archive.string())
                                         : "sha256sum " + quote(archive.string());
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return false;
        char buf[128] = {0};
        bool ok = std::fscanf(p, "%127s", buf) == 1 && pkg.sha256 == buf;
        pclose(p);
        return ok;
    };

    if (checksum_matches()) {
        report("  " + pkg.name + " " + pkg.version + ": already downloaded");
        return true;
    }

    report("  downloading " + pkg.name + " " + pkg.version);
    if (std::system(("curl --fail --location --progress-bar -o " +
                     quote(archive.string()) + " " + quote(pkg.url)).c_str()) != 0) {
        error = "failed to download " + pkg.url;
        return false;
    }
    if (!checksum_matches()) {
        error = "checksum mismatch for " + archive_name(pkg.url) +
                " -- refusing to build from it";
        fs::remove(archive);
        return false;
    }
    return true;
}

bool unpack_source(const SourcePackage& pkg, const fs::path& dir, std::string& error,
                   const std::function<void(const std::string&)>& report) {
    if (fs::exists(dir / pkg.unpacks_to)) {
        report("  " + pkg.name + ": already unpacked");
        return true;
    }
    report("  unpacking " + pkg.name);
    if (run_in(dir, "tar -xf " + quote(archive_name(pkg.url))) != 0) {
        error = "failed to unpack " + archive_name(pkg.url);
        return false;
    }
    return true;
}

} // namespace

const std::vector<SourcePackage>& toolchain_sources() {
    static const std::vector<SourcePackage> srcs = {
        {"binutils", "2.47",
         "https://ftpmirror.gnu.org/gnu/binutils/binutils-2.47.tar.bz2",
         "3068128c75cda9f898ccb4211d360246e8e195ffcc9dfb655b23ae23a54800e8",
         "binutils-2.47"},
        {"gcc", "16.1.0",
         "https://ftpmirror.gnu.org/gnu/gcc/gcc-16.1.0/gcc-16.1.0.tar.xz",
         "50efb4d94c3397aff3b0d61a5abd748b4dd31d9d3f2ab7be05b171d36a510f79",
         "gcc-16.1.0"},
        {"avr-libc", "2.3.2",
         "https://github.com/avrdudes/avr-libc/releases/download/"
         "avr-libc-2_3_2-release/avr-libc-2.3.2.tar.bz2",
         "92eb253d30cec94f2861a82d40d0b17ad79c0d95fce32b74ffccc26a70afb150",
         "avr-libc-2.3.2"},
    };
    return srcs;
}

std::vector<std::string> binutils_configure_args(const std::string& prefix) {
    return {
        "--target=avr",
        "--prefix=" + prefix,
        "--disable-nls",        // no translations; smaller and faster to build
        "--disable-werror",     // upstream warnings must not fail our build
        "--disable-multilib",
    };
}

std::vector<std::string> gcc_configure_args(const std::string& prefix) {
    return {
        "--target=avr",
        "--prefix=" + prefix,
        "--enable-languages=c,c++",
        "--disable-nls",
        "--disable-libssp",     // stack protector needs an OS
        "--disable-libada",
        "--disable-shared",     // freestanding target: static only
        "--disable-threads",    // no threading model on bare metal
        "--disable-libgomp",
        "--disable-libstdcxx",  // no hosted C++ runtime on AVR
        "--with-dwarf2",
        "--disable-multilib",
    };
}

std::vector<std::string> avr_libc_configure_args(const std::string& prefix) {
    return {
        "--prefix=" + prefix,
        "--host=avr",
    };
}

FetchResult build_avr_toolchain(const std::string& prefix, const std::string& work_dir,
                                const std::function<void(const std::string&)>& progress) {
    FetchResult result;
    auto report = [&](const std::string& m) { if (progress) progress(m); };

    for (const char* tool : {"curl", "tar", "make"}) {
        if (!have(tool)) {
            result.error = std::string(tool) + " is required to build a toolchain "
                           "but was not found on PATH";
            return result;
        }
    }
    if (!have("cc") && !have("gcc") && !have("clang")) {
        result.error = "a host C compiler is required to build a toolchain";
        return result;
    }

    std::error_code ec;
    fs::path work(work_dir);
    fs::create_directories(work, ec);
    if (ec) {
        result.error = "cannot create " + work.string() + ": " + ec.message();
        return result;
    }
    fs::create_directories(prefix, ec);

    // Later stages need the binaries built by earlier ones.
    std::string path_prefix = "PATH=" + quote((fs::path(prefix) / "bin").string()) +
                              ":$PATH ";
    std::string jobs = "-j$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)";

    const auto& srcs = toolchain_sources();
    for (const SourcePackage& pkg : srcs) {
        std::string error;
        if (!fetch_source(pkg, work, error, report))  { result.error = error; return result; }
        if (!unpack_source(pkg, work, error, report)) { result.error = error; return result; }
    }

    // ---- stage 1: binutils -------------------------------------------------
    {
        report("building binutils (assembler and linker for AVR)");
        fs::path build = work / "build-binutils";
        fs::create_directories(build, ec);
        std::string configure = "../" + srcs[0].unpacks_to + "/configure" +
                                join(binutils_configure_args(prefix));
        if (run_in(build, configure) != 0 ||
            run_in(build, "make " + jobs) != 0 ||
            run_in(build, "make install") != 0) {
            result.error = "binutils build failed (see the output above)";
            return result;
        }
    }

    // ---- stage 2: gcc ------------------------------------------------------
    {
        report("building gcc (this is the long one)");
        // GCC needs GMP, MPFR and MPC. Its own script places verified copies
        // in the source tree, which is more reliable than hunting for system
        // versions that may not match.
        if (run_in(work / srcs[1].unpacks_to, "./contrib/download_prerequisites") != 0) {
            result.error = "could not fetch gcc's prerequisites (gmp/mpfr/mpc)";
            return result;
        }
        fs::path build = work / "build-gcc";
        fs::create_directories(build, ec);
        std::string configure = path_prefix + "../" + srcs[1].unpacks_to + "/configure" +
                                join(gcc_configure_args(prefix));
        if (run_in(build, configure) != 0 ||
            run_in(build, path_prefix + "make " + jobs) != 0 ||
            run_in(build, path_prefix + "make install") != 0) {
            result.error = "gcc build failed (see the output above)";
            return result;
        }
    }

    // ---- stage 3: avr-libc -------------------------------------------------
    {
        report("building avr-libc (the C library, compiled by the avr-gcc above)");
        fs::path src = work / srcs[2].unpacks_to;
        std::string configure = path_prefix + "./configure" +
                                join(avr_libc_configure_args(prefix)) +
                                " --build=$(./config.guess)";
        if (run_in(src, configure) != 0 ||
            run_in(src, path_prefix + "make " + jobs) != 0 ||
            run_in(src, path_prefix + "make install") != 0) {
            result.error = "avr-libc build failed (see the output above)";
            return result;
        }
    }

    result.ok = true;
    result.installed_to = prefix;
    return result;
}

} // namespace ardio
