#include "ardio/platform/ports.h"

namespace ardio {
namespace {

std::string list_devices(const std::vector<PortInfo>& ports) {
    std::string s;
    for (const PortInfo& p : ports) {
        s += "\n  " + p.device;
        if (!p.description.empty()) s += "  (" + p.description + ")";
    }
    return s;
}

} // namespace

PortSelection select_port(const std::vector<PortInfo>& ports,
                          std::string_view wanted_port,
                          std::string_view wanted_board) {
    PortSelection sel;

    // An explicitly named board must exist, whatever else happens.
    const Board* forced_board = nullptr;
    if (!wanted_board.empty()) {
        forced_board = find_board_by_id(wanted_board);
        if (!forced_board) {
            sel.error = "unknown board '" + std::string(wanted_board) +
                        "'. Run 'ardio boards' to see supported targets.";
            return sel;
        }
    }

    if (ports.empty()) {
        sel.error = "no serial ports found. Is the board plugged in? "
                    "A CH340 board also needs its USB-serial driver installed.";
        return sel;
    }

    // Pick the port.
    if (!wanted_port.empty()) {
        for (const PortInfo& p : ports)
            if (p.device == wanted_port) { sel.port = &p; break; }
        if (!sel.port) {
            sel.error = "no such port '" + std::string(wanted_port) +
                        "'. Ports found:" + list_devices(ports);
            return sel;
        }
    } else {
        // Auto-detect: prefer ports whose USB ID matches a known board.
        std::vector<const PortInfo*> candidates;
        for (const PortInfo& p : ports)
            if (p.has_usb_id && !find_boards_by_usb(p.usb).empty())
                candidates.push_back(&p);

        if (candidates.size() == 1) {
            sel.port = candidates[0];
        } else if (candidates.size() > 1) {
            std::string s;
            for (const PortInfo* p : candidates)
                s += "\n  " + p->device + "  (" + p->description + ")";
            sel.error = "several boards connected -- pass --port to choose:" + s;
            return sel;
        } else if (ports.size() == 1) {
            sel.port = &ports[0];   // sole port, unrecognised ID
        } else {
            sel.error = "could not identify a board -- pass --port to choose:" +
                        list_devices(ports);
            return sel;
        }
    }

    // Pick the board.
    if (forced_board) {
        sel.board = forced_board;
        return sel;
    }

    std::vector<const Board*> matches;
    if (sel.port->has_usb_id) matches = find_boards_by_usb(sel.port->usb);

    if (matches.size() == 1) {
        sel.board = matches[0];
    } else if (matches.size() > 1) {
        std::string names;
        for (const Board* b : matches) names += "\n  " + b->id + "  (" + b->name + ")";
        sel.error = "USB ID matches several boards -- pass --board:" + names;
        sel.port = nullptr;
    } else {
        sel.error = "unrecognised device on " + sel.port->device +
                    " -- pass --board to say what it is. "
                    "Run 'ardio boards' to see supported targets.";
        sel.port = nullptr;
    }
    return sel;
}

} // namespace ardio
