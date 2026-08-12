#pragma once
#include <functional>
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

// ---------------------------------------------------------------- fetching --

// A downloadable toolchain archive, pinned by checksum.
struct RemoteTool {
    std::string package;    // "avr"
    std::string host;       // "x86_64-apple-darwin14"
    std::string version;
    std::string url;
    std::string sha256;     // lowercase hex, 64 chars
    long long size_bytes = 0;
    std::string unpacks_to; // top-level directory inside the archive
};

const std::vector<RemoteTool>& remote_tool_registry();

// Maps a (uname -s, uname -m) pair to the host triple used by the registry.
// Returns an empty string for hosts with no published build.
std::string host_triple_for(std::string_view os, std::string_view machine);

// host_triple_for() applied to the running machine.
std::string host_triple();

const RemoteTool* find_remote_tool(std::string_view package, std::string_view host);

struct FetchResult {
    bool ok = false;
    std::string error;
    std::string installed_to;
};

// Downloads, verifies, and unpacks `tool` under `dest_root`. Requires the
// caller to have already obtained the user's consent -- this function does not
// prompt. `progress` may be nullptr.
FetchResult fetch_remote_tool(const RemoteTool& tool, const std::string& dest_root,
                              const std::function<void(const std::string&)>& progress);

} // namespace ardio
