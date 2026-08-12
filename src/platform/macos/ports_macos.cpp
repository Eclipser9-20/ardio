#include "ardio/platform/ports.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>

namespace ardio {
namespace {

std::string cf_string_to_std(CFStringRef s) {
    if (!s) return {};
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) return buf;
    return {};
}

// Walks up the IORegistry from a serial service to the USB device that owns
// it, so we can read idVendor/idProduct. Not every serial port has one
// (Bluetooth ports, the debug console).
bool usb_ids_for_service(io_object_t service, UsbId& out, std::string& description) {
    io_registry_entry_t node = service;
    IOObjectRetain(node);

    bool found = false;
    for (int depth = 0; depth < 8 && node; ++depth) {
        CFTypeRef vid = IORegistryEntryCreateCFProperty(node, CFSTR("idVendor"),
                                                        kCFAllocatorDefault, 0);
        CFTypeRef pid = IORegistryEntryCreateCFProperty(node, CFSTR("idProduct"),
                                                        kCFAllocatorDefault, 0);
        if (vid && pid) {
            int v = 0, p = 0;
            CFNumberGetValue(CFNumberRef(vid), kCFNumberIntType, &v);
            CFNumberGetValue(CFNumberRef(pid), kCFNumberIntType, &p);
            out = UsbId{uint16_t(v), uint16_t(p)};
            CFTypeRef name = IORegistryEntryCreateCFProperty(
                node, CFSTR("USB Product Name"), kCFAllocatorDefault, 0);
            if (name) {
                description = cf_string_to_std(CFStringRef(name));
                CFRelease(name);
            }
            found = true;
        }
        if (vid) CFRelease(vid);
        if (pid) CFRelease(pid);
        if (found) break;

        io_registry_entry_t parent = 0;
        if (IORegistryEntryGetParentEntry(node, kIOServicePlane, &parent) != KERN_SUCCESS)
            break;
        IOObjectRelease(node);
        node = parent;
    }
    if (node) IOObjectRelease(node);
    return found;
}

} // namespace

std::vector<PortInfo> enumerate_ports() {
    std::vector<PortInfo> out;

    CFMutableDictionaryRef matching = IOServiceMatching(kIOSerialBSDServiceValue);
    if (!matching) return out;
    CFDictionarySetValue(matching, CFSTR(kIOSerialBSDTypeKey),
                         CFSTR(kIOSerialBSDAllTypes));

    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &it) != KERN_SUCCESS)
        return out;

    io_object_t service;
    while ((service = IOIteratorNext(it))) {
        CFTypeRef path = IORegistryEntryCreateCFProperty(
            service, CFSTR(kIOCalloutDeviceKey), kCFAllocatorDefault, 0);
        if (path) {
            PortInfo info;
            info.device = cf_string_to_std(CFStringRef(path));
            CFRelease(path);

            // Skip the ports that are never a board.
            if (info.device.find("Bluetooth") == std::string::npos &&
                info.device.find("debug-console") == std::string::npos) {
                info.has_usb_id = usb_ids_for_service(service, info.usb, info.description);
                out.push_back(info);
            }
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(it);
    return out;
}

} // namespace ardio
