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
