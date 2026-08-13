#pragma once
#include "ardio/avr/device.h"
#include <fstream>
#include <sstream>
#include <string>

// Reads a runtime assembly file the way the build does.
//
// The runtime is no longer written for one chip. Its register addresses and
// pin numbering arrive as AD_* constants and two generated tables, rendered
// from a device description, so a .S file on its own is not assemblable source
// -- every one of those names would be undefined. The build appends the device
// prelude before handing the text to the assembler, and a test that means to
// assemble the real runtime has to do the same or it is testing something that
// never exists in practice.
//
// The prelude goes last for the same reason it does in the build: it emits the
// two flash tables, and word address 0 is the reset vector, so putting it first
// would land the pin map on the entry point. Forward references resolve because
// the assembler makes two passes.
namespace ardio::test {

// Finds a file relative to the repository root, wherever the test binary was
// run from.
inline bool read_relative(const std::string& relative, std::string& out) {
    static const char* const prefixes[] = {"", "../", "../../", "../../../"};
    for (const char* prefix : prefixes) {
        std::ifstream in(prefix + relative, std::ios::binary);
        if (!in) continue;
        std::ostringstream buf;
        buf << in.rdbuf();
        out = buf.str();
        return true;
    }
    return false;
}

// The device every runtime test assembles against unless it says otherwise.
// The ATmega328P is the part ardio has actually flashed and verified on real
// hardware, which makes it the honest default for "does the runtime build".
inline std::string default_prelude() {
    const avr::AvrDevice* device = avr::find_device("atmega328p");
    if (!device) return {};
    return avr::device_prelude(*device, 16000000);
}

// Reads a runtime .S and appends the prelude, yielding text the assembler can
// actually consume.
inline bool read_runtime(const std::string& relative, std::string& out) {
    if (!read_relative(relative, out)) return false;
    out += "\n";
    out += default_prelude();
    return true;
}

} // namespace ardio::test
