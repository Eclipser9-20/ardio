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
