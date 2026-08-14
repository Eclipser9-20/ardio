#include "ardio/platform/usb_serial.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace ardio {
namespace {

// The bridges ardio can configure itself.
//
// A board is usually built around one of a handful of chips, and each has its
// own vendor control requests for baud rate and line state. There is no
// generic USB-serial class here to fall back on -- CDC-ACM exists, but almost
// no hobbyist board uses it -- so support means knowing the specific chip.
const std::vector<KnownBridge> kBridges = {
    {0x1A86, 0x7523, "ch34x",  "CH340"},
    {0x1A86, 0x5523, "ch34x",  "CH341"},
    {0x1A86, 0x55D4, "ch34x",  "CH9102"},
    {0x10C4, 0xEA60, "cp210x", "CP2102"},
    {0x10C4, 0xEA70, "cp210x", "CP2105"},
    {0x0403, 0x6001, "ftdi",   "FT232R"},
    {0x0403, 0x6015, "ftdi",   "FT231X"},
    {0x2341, 0x0043, "cdc",    "Arduino Uno"},
    {0x2341, 0x0001, "cdc",    "Arduino Uno (8U2)"},
};

std::string cf_string(CFTypeRef ref) {
    if (!ref || CFGetTypeID(ref) != CFStringGetTypeID()) return {};
    char buf[256];
    if (!CFStringGetCString((CFStringRef)ref, buf, sizeof buf, kCFStringEncodingUTF8))
        return {};
    return buf;
}

uint32_t cf_number(CFTypeRef ref) {
    if (!ref || CFGetTypeID(ref) != CFNumberGetTypeID()) return 0;
    uint32_t value = 0;
    CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt32Type, &value);
    return value;
}

std::string property_string(io_service_t service, const char* key) {
    CFTypeRef ref = IORegistryEntryCreateCFProperty(
        service, CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8),
        kCFAllocatorDefault, 0);
    std::string out = cf_string(ref);
    if (ref) CFRelease(ref);
    return out;
}

uint32_t property_number(io_service_t service, const char* key) {
    CFTypeRef ref = IORegistryEntryCreateCFProperty(
        service, CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8),
        kCFAllocatorDefault, 0);
    uint32_t out = cf_number(ref);
    if (ref) CFRelease(ref);
    return out;
}

// A USB bridge driven directly, with no kernel driver in the path.
class UsbSerialPort : public SerialPort {
public:
    ~UsbSerialPort() override { close(); }

    bool open(const std::string& spec, int baud, std::string& error) override {
        close();

        unsigned vid = 0, pid = 0;
        if (std::sscanf(spec.c_str(), "%x:%x", &vid, &pid) != 2) {
            error = "expected a usb id like '1a86:7523', got '" + spec + "'";
            return false;
        }

        const KnownBridge* bridge = find_bridge(uint16_t(vid), uint16_t(pid));
        if (!bridge) {
            error = "ardio does not know how to configure the bridge at " + spec +
                    ". Run 'ardio usb' to see what it does support.";
            return false;
        }
        chip_ = bridge->chip;

        if (!open_device(uint16_t(vid), uint16_t(pid), error)) return false;
        if (!claim_interface(error)) { close(); return false; }
        if (!configure(baud, error))  { close(); return false; }
        return true;
    }

    void close() override {
        if (interface_) {
            (*interface_)->USBInterfaceClose(interface_);
            (*interface_)->Release(interface_);
            interface_ = nullptr;
        }
        if (device_) {
            (*device_)->USBDeviceClose(device_);
            (*device_)->Release(device_);
            device_ = nullptr;
        }
    }

    bool write(const uint8_t* data, size_t len) override {
        if (!interface_) return false;
        IOReturn rc = (*interface_)->WritePipeTO(interface_, out_pipe_,
                                                 const_cast<uint8_t*>(data),
                                                 uint32_t(len), 1000, 2000);
        return rc == kIOReturnSuccess;
    }

    size_t read(uint8_t* out, size_t max, int timeout_ms) override {
        if (!interface_) return 0;

        // Anything already pulled off the wire is served first. A bulk read
        // returns whole packets, so a caller asking for one byte would
        // otherwise throw the rest of the packet away.
        if (!pending_.empty()) return take_pending(out, max);

        uint8_t buffer[512];
        uint32_t size = sizeof buffer;
        IOReturn rc = (*interface_)->ReadPipeTO(interface_, in_pipe_, buffer, &size,
                                                uint32_t(timeout_ms < 0 ? 0 : timeout_ms),
                                                uint32_t(timeout_ms < 0 ? 0 : timeout_ms + 500));
        if (rc != kIOReturnSuccess || size == 0) return 0;

        size_t offset = 0;
        // The CH34x prefixes each packet with two status bytes that are not
        // part of the stream. Passing them through would corrupt every reply a
        // bootloader sends.
        if (chip_ == "ch34x" && size >= 2) offset = 2;

        pending_.assign(buffer + offset, buffer + size);
        return take_pending(out, max);
    }

    // DTR and RTS are what reset an Arduino and what select the ROM loader on
    // an ESP, so they are not optional decoration -- without them the board is
    // never in its bootloader when we start talking.
    void set_dtr(bool level) override { dtr_ = level; apply_lines(); }
    void set_rts(bool level) override { rts_ = level; apply_lines(); }

private:
    size_t take_pending(uint8_t* out, size_t max) {
        size_t n = pending_.size() < max ? pending_.size() : max;
        std::memcpy(out, pending_.data(), n);
        pending_.erase(pending_.begin(), pending_.begin() + long(n));
        return n;
    }

    bool open_device(uint16_t vid, uint16_t pid, std::string& error) {
        CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
        if (!match) { error = "cannot build a USB matching dictionary"; return false; }

        io_iterator_t iter = 0;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) !=
            kIOReturnSuccess) {
            error = "cannot query the USB bus";
            return false;
        }

        io_service_t service;
        bool found = false;
        while ((service = IOIteratorNext(iter))) {
            if (property_number(service, "idVendor") == vid &&
                property_number(service, "idProduct") == pid) {
                found = open_service(service, error);
                IOObjectRelease(service);
                break;
            }
            IOObjectRelease(service);
        }
        IOObjectRelease(iter);

        if (!found && error.empty())
            error = "no USB device matching that id is attached";
        return found;
    }

    bool open_service(io_service_t service, std::string& error) {
        IOCFPlugInInterface** plugin = nullptr;
        SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                              kIOCFPlugInInterfaceID, &plugin,
                                              &score) != kIOReturnSuccess ||
            !plugin) {
            error = "cannot create a plug-in for the USB device";
            return false;
        }

        HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
            (LPVOID*)&device_);
        (*plugin)->Release(plugin);
        if (hr != S_OK || !device_) {
            error = "cannot obtain the USB device interface";
            return false;
        }

        IOReturn rc = (*device_)->USBDeviceOpen(device_);
        if (rc == kIOReturnExclusiveAccess) {
            // A kernel driver already owns it, which means a /dev entry exists
            // and should be used instead. Saying so beats failing obscurely.
            error = "that device is already claimed by a system driver, so it "
                    "has a /dev entry -- use -port instead of the USB id";
            return false;
        }
        if (rc != kIOReturnSuccess) {
            error = "cannot open the USB device (is it in use?)";
            return false;
        }

        // Configuration 1 is what every bridge here uses. Without setting it
        // the device has no interfaces to claim.
        UInt8 config = 0;
        (*device_)->GetConfiguration(device_, &config);
        if (config != 1) (*device_)->SetConfiguration(device_, 1);
        return true;
    }

    bool claim_interface(std::string& error) {
        IOUSBFindInterfaceRequest request;
        request.bInterfaceClass = kIOUSBFindInterfaceDontCare;
        request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
        request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
        request.bAlternateSetting = kIOUSBFindInterfaceDontCare;

        io_iterator_t iter = 0;
        if ((*device_)->CreateInterfaceIterator(device_, &request, &iter) !=
            kIOReturnSuccess) {
            error = "cannot iterate the device's interfaces";
            return false;
        }

        io_service_t service = IOIteratorNext(iter);
        IOObjectRelease(iter);
        if (!service) { error = "the device exposes no interfaces"; return false; }

        IOCFPlugInInterface** plugin = nullptr;
        SInt32 score = 0;
        IOReturn rc = IOCreatePlugInInterfaceForService(
            service, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID,
            &plugin, &score);
        IOObjectRelease(service);
        if (rc != kIOReturnSuccess || !plugin) {
            error = "cannot create a plug-in for the interface";
            return false;
        }

        HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),
            (LPVOID*)&interface_);
        (*plugin)->Release(plugin);
        if (hr != S_OK || !interface_) {
            error = "cannot obtain the USB interface";
            return false;
        }

        if ((*interface_)->USBInterfaceOpen(interface_) != kIOReturnSuccess) {
            error = "cannot open the USB interface";
            return false;
        }

        // Find the bulk endpoints. Their numbers differ between chips, so they
        // are discovered rather than assumed.
        UInt8 count = 0;
        (*interface_)->GetNumEndpoints(interface_, &count);
        for (UInt8 i = 1; i <= count; ++i) {
            UInt8 direction = 0, number = 0, type = 0, interval = 0;
            UInt16 packet = 0;
            if ((*interface_)->GetPipeProperties(interface_, i, &direction, &number,
                                                 &type, &packet, &interval) !=
                kIOReturnSuccess)
                continue;
            if (type != kUSBBulk) continue;
            if (direction == kUSBIn && in_pipe_ == 0) in_pipe_ = i;
            if (direction == kUSBOut && out_pipe_ == 0) out_pipe_ = i;
        }

        if (in_pipe_ == 0 || out_pipe_ == 0) {
            error = "the interface has no bulk endpoints in both directions";
            return false;
        }
        return true;
    }

    IOReturn control(uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                     void* data = nullptr, uint16_t length = 0) {
        IOUSBDevRequest req{};
        req.bmRequestType = type;
        req.bRequest = request;
        req.wValue = value;
        req.wIndex = index;
        req.wLength = length;
        req.pData = data;
        return (*device_)->DeviceRequest(device_, &req);
    }

    bool configure(int baud, std::string& error) {
        baud_ = baud;
        if (chip_ == "cp210x") {
            // Enable the UART, then set the rate. The CP210x takes the rate as
            // a plain 32-bit value rather than a divisor, which is why this is
            // the simplest of the three.
            control(0x41, 0x00, 0x0001, 0);
            uint32_t rate = uint32_t(baud);
            control(0x41, 0x1E, 0, 0, &rate, sizeof rate);
            return true;
        }
        if (chip_ == "ch34x") {
            // The CH34x wants its own initialisation sequence before it will
            // pass data. The divisor encoding below covers the common rates;
            // an unusual rate would need the full table from the datasheet.
            control(0x40, 0xA1, 0, 0);
            uint16_t divisor = ch34x_divisor(baud);
            control(0x40, 0x9A, 0x1312, divisor);
            control(0x40, 0x9A, 0x2518, 0x00C3);   // 8 data bits, 1 stop, no parity
            control(0x40, 0xA1, 0x501F, 0xD90A);
            return true;
        }
        if (chip_ == "ftdi") {
            control(0x40, 0x00, 0x0000, 0);        // reset
            control(0x40, 0x03, ftdi_divisor(baud), 0);
            control(0x40, 0x04, 0x0008, 0);        // 8N1
            return true;
        }
        error = "no configuration routine for chip '" + chip_ + "'";
        return false;
    }

    static uint16_t ch34x_divisor(int baud) {
        switch (baud) {
        case 115200: return 0xCC83;
        case 57600:  return 0x9864;
        case 19200:  return 0xB281;
        case 9600:   return 0xB201;
        default:     return 0xCC83;
        }
    }

    static uint16_t ftdi_divisor(int baud) {
        // The FTDI clock is 3 MHz with a 3-bit fractional part; 115200 and
        // 57600 are the rates a bootloader uses, so they are given exactly.
        switch (baud) {
        case 115200: return 0x001A;
        case 57600:  return 0x0034;
        case 19200:  return 0x009C;
        case 9600:   return 0x4138;
        default:     return 0x001A;
        }
    }

    void apply_lines() {
        if (!device_) return;
        if (chip_ == "cp210x") {
            // Low byte is the levels, high byte says which of them to apply.
            uint16_t value = uint16_t((dtr_ ? 0x0001 : 0) | (rts_ ? 0x0002 : 0) | 0x0300);
            control(0x41, 0x07, value, 0);
        } else if (chip_ == "ch34x") {
            // Active low, and the register wants the inverse of the levels.
            uint16_t value = 0x00;
            if (!dtr_) value |= 0x20;
            if (!rts_) value |= 0x40;
            control(0x40, 0xA4, uint16_t(~value & 0xFF), 0);
        } else if (chip_ == "ftdi") {
            control(0x40, 0x01, uint16_t(dtr_ ? 0x0101 : 0x0100), 0);
            control(0x40, 0x01, uint16_t(rts_ ? 0x0202 : 0x0200), 0);
        }
    }

    IOUSBDeviceInterface**    device_ = nullptr;
    IOUSBInterfaceInterface** interface_ = nullptr;
    uint8_t in_pipe_ = 0, out_pipe_ = 0;
    std::string chip_;
    std::vector<uint8_t> pending_;
    int baud_ = 0;
    bool dtr_ = false, rts_ = false;
};

} // namespace

const std::vector<KnownBridge>& known_bridges() { return kBridges; }

const KnownBridge* find_bridge(uint16_t vid, uint16_t pid) {
    for (const KnownBridge& b : kBridges)
        if (b.vid == vid && b.pid == pid) return &b;
    return nullptr;
}

std::vector<UsbDeviceInfo> enumerate_usb_devices(std::string& error) {
    std::vector<UsbDeviceInfo> out;

    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) { error = "cannot build a USB matching dictionary"; return out; }

    io_iterator_t iter = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) !=
        kIOReturnSuccess) {
        error = "cannot query the USB bus";
        return out;
    }

    io_service_t service;
    while ((service = IOIteratorNext(iter))) {
        UsbDeviceInfo info;
        info.vid = uint16_t(property_number(service, "idVendor"));
        info.pid = uint16_t(property_number(service, "idProduct"));
        info.location = property_number(service, "locationID");
        info.product = property_string(service, "USB Product Name");
        info.manufacturer = property_string(service, "USB Vendor Name");
        info.serial = property_string(service, "USB Serial Number");

        if (const KnownBridge* bridge = find_bridge(info.vid, info.pid)) {
            info.supported = true;
            info.chip = bridge->chip;
        }
        out.push_back(info);
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    return out;
}

std::unique_ptr<SerialPort> make_usb_serial_port() {
    return std::make_unique<UsbSerialPort>();
}

} // namespace ardio
