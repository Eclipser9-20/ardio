#pragma once
#include "ardio/platform/serial.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Talking to a USB-serial bridge directly, without a kernel driver.
//
// The usual path to a board is a virtual COM port: the operating system loads
// a driver for the bridge chip, which publishes a /dev/cu.* device, and ardio
// opens that. When no such driver is present there is no /dev entry, and every
// tool that only knows how to open one is stuck -- even though the chip is
// sitting there able to talk.
//
// This is the other path. ardio opens the USB device itself, configures the
// bridge with the vendor control requests its datasheet defines, and moves
// bytes over the bulk endpoints. It is the same work the kernel driver would
// have done, done in ardio instead, which means a board is reachable whether
// or not the host happens to ship support for its particular bridge.
namespace ardio {

// A USB device as the bus reports it, whether or not any driver claimed it.
struct UsbDeviceInfo {
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string product;
    std::string manufacturer;
    std::string serial;
    uint32_t location = 0;

    // True when ardio recognises this as a bridge it can drive.
    bool supported = false;
    std::string chip;      // "ch34x", "cp210x", "ftdi", or empty
};

// Everything on the bus. This asks the USB stack directly rather than looking
// for /dev entries, so a device with no driver still shows up -- which makes
// it the honest answer to "is the board actually there".
std::vector<UsbDeviceInfo> enumerate_usb_devices(std::string& error);

// A SerialPort backed by a USB bridge ardio drives itself.
//
// `open` takes "vid:pid" in hex, e.g. "1a86:7523", rather than a device path,
// because there is no path -- that is the entire point. When several devices
// share a vid:pid the first is used.
std::unique_ptr<SerialPort> make_usb_serial_port();

// Which bridges ardio knows how to configure. Exposed so the CLI can say what
// it supports rather than only reporting what it does not.
struct KnownBridge {
    uint16_t vid;
    uint16_t pid;
    const char* chip;
    const char* name;
};
const std::vector<KnownBridge>& known_bridges();
const KnownBridge* find_bridge(uint16_t vid, uint16_t pid);

} // namespace ardio
