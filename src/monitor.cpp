#include "ardio/monitor.h"
#include <cstdio>

namespace ardio {

void run_monitor(SerialPort& port, volatile bool& stop_flag) {
    uint8_t buf[512];
    while (!stop_flag) {
        size_t n = port.read(buf, sizeof(buf), 100);
        if (n > 0) {
            std::fwrite(buf, 1, n, stdout);
            std::fflush(stdout);
        }
    }
}

} // namespace ardio
