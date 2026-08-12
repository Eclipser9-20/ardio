#pragma once
#include "ardio/platform/serial.h"

namespace ardio {

// Reads from `port` and writes to stdout until `stop_flag` becomes true.
void run_monitor(SerialPort& port, volatile bool& stop_flag);

} // namespace ardio
