#pragma once
#include "ardio/platform/serial.h"
#include <memory>
#include <string>

namespace ardio {

// A serial port at the far end of an ssh connection, for a board wired to a
// remote host's UART.
//
// The remote end runs `stty` and `cat` and nothing else, so no software has to
// be installed there. `reset_gpio` and `boot_gpio` name GPIO lines on the host
// wired to the board's RESET and, for an ESP, GPIO0; a header UART has no
// modem control lines, so without them the board cannot be put into its
// bootloader automatically and must be reset by hand.
std::unique_ptr<SerialPort> make_remote_serial_port(const std::string& host,
                                                    const std::string& gpio_chip,
                                                    int reset_gpio, int boot_gpio);

} // namespace ardio
