// Choosing an execution core for the host.
//
// Two cores answer the same interface and are meant to be interchangeable: the
// portable reference interpreter, which is the oracle everything else is
// checked against, and a native translator that turns AVR basic blocks into
// host code. The difference between them is several orders of magnitude in
// speed, so which one a caller got is not an implementation detail -- it is the
// difference between a sketch second taking a sketch second and taking a
// minute. That is why Core::description() exists and why nothing here wraps or
// paraphrases it: the core the caller receives describes itself, so the
// explanation can never drift away from what is actually running.
//
// make_reference_core is NOT defined here. It is defined next to the
// interpreter it builds, in core_reference.cpp, which is also where its
// implementation class lives.

#include "ardio/emu/machine.h"

#include <memory>

namespace ardio::emu {

// Defined in core_arm64.cpp. Declared here rather than in a header because the
// translator is an implementation detail of core selection: nothing should be
// able to ask for it by name and bypass the fallback.
std::unique_ptr<Core> make_arm64_core(const avr::AvrDevice& device);

namespace {

// The native translator, when there is one.
//
// There is no AVR-to-host translator yet -- src/emu/core_arm64.cpp is still
// empty -- so this reports "no native core on this host" by returning null, and
// make_core falls back. The fallback is on the emptiness of this function
// rather than on a preprocessor test of the host architecture on purpose: a
// translator that exists but cannot handle a particular device description
// needs to be able to decline the same way, and an #ifdef cannot decline.
//
// Adopting the translator is then one line here -- return make_arm64_core(...)
// instead of null, with its declaration above -- and no change anywhere else,
// because everything downstream already goes through Core.
std::unique_ptr<Core> make_native_core(const avr::AvrDevice& device) {
    // Returns null on a host that is not ARM64, and also when the executable
    // memory it needs is refused -- a sandbox or a hardened runtime without
    // the JIT entitlement will do that. Both cases fall through to the
    // reference core rather than failing, so emulation always works
    // somewhere; the difference is speed, and --explain reports which one the
    // caller actually got.
    return make_arm64_core(device);
}

} // namespace

std::unique_ptr<Core> make_core(const avr::AvrDevice& device) {
    if (std::unique_ptr<Core> native = make_native_core(device)) return native;
    return make_reference_core(device);
}

} // namespace ardio::emu
