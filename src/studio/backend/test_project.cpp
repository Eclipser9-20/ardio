// Self-test for the .ardioproj backend: scaffold a project, verify its tree and
// manifest, and check that the formatted manifest round-trips through the
// parser unchanged. Prints the tree and the generated manifest so a human can
// see the "advanced and cool" formatting. Exits non-zero on the first failure.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "studio/backend/project.h"

#ifndef PROJECT_TEST_TMP
#define PROJECT_TEST_TMP "."
#endif

namespace fs = std::filesystem;
using namespace studio;

static int failures = 0;
#define CHECK(cond, msg)                            \
    do {                                            \
        if (!(cond)) {                              \
            std::printf("FAIL: %s\n", msg);         \
            ++failures;                             \
        }                                           \
    } while (0)

static void print_tree(const std::string& root) {
    fs::path base(root);
    std::printf("%s/\n", base.filename().string().c_str());
    std::vector<fs::path> all;
    for (auto& e : fs::recursive_directory_iterator(root)) all.push_back(e.path());
    std::sort(all.begin(), all.end());
    for (auto& p : all) {
        fs::path rel = fs::relative(p, base);
        int depth = static_cast<int>(std::distance(rel.begin(), rel.end()));
        std::string indent(static_cast<size_t>(depth) * 2, ' ');
        std::printf("%s%s%s\n", indent.c_str(), p.filename().string().c_str(),
                    fs::is_directory(p) ? "/" : "");
    }
}

int main() {
    std::string tmp = PROJECT_TEST_TMP;
    std::string root = tmp + "/ExampleProject.ardioproj";
    std::error_code ec;
    fs::remove_all(root, ec);  // clean slate

    Project proj;
    std::string err;
    CHECK(create_project(tmp, "ExampleProject", "nano", proj, err), err.c_str());

    // The tree matches the spec.
    CHECK(fs::exists(root + "/project.ardiomanif"), "manifest missing");
    CHECK(fs::exists(root + "/src/ExampleProject.ino"), "src sketch missing");
    CHECK(fs::exists(root + "/include/ExampleProject.hpp"), "include header missing");
    CHECK(fs::is_directory(root + "/build/Release"), "build/Release missing");

    // Reopen and check the parsed fields.
    Project reopened;
    CHECK(open_project(root, reopened, err), err.c_str());
    CHECK(reopened.manifest.name == "ExampleProject", "name mismatch");
    CHECK(reopened.manifest.board == "nano", "board mismatch");
    CHECK(reopened.manifest.main == "src/ExampleProject.ino", "main mismatch");
    CHECK(reopened.manifest.build_output == "build/Release/ExampleProject.hex", "output mismatch");
    CHECK(reopened.hex_path() == root + "/build/Release/ExampleProject.hex", "hex_path wrong");

    // Round-trip: serialize -> parse -> same values (comments/inline notes and
    // alignment must not confuse the reader).
    Manifest again = Manifest::parse(proj.manifest.serialize());
    CHECK(again.name == proj.manifest.name, "roundtrip name");
    CHECK(again.board == proj.manifest.board, "roundtrip board");
    CHECK(again.optimize == proj.manifest.optimize, "roundtrip optimize");
    CHECK(again.upload_baud == proj.manifest.upload_baud, "roundtrip baud");
    CHECK(again.upload_protocol == proj.manifest.upload_protocol, "roundtrip protocol");
    CHECK(again.monitor_baud == proj.manifest.monitor_baud, "roundtrip monitor baud");

    std::printf("\n== .ardioproj tree ==\n");
    print_tree(root);
    std::printf("\n== project.ardiomanif ==\n%s\n", proj.manifest.serialize().c_str());

    if (failures == 0) std::printf("project backend: all checks passed.\n");
    else std::printf("project backend: %d checks FAILED.\n", failures);
    return failures ? 1 : 0;
}
