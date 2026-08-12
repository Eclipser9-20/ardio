#include "ardio/cli.h"

int main(int argc, char** argv) {
    return ardio::run_command(ardio::parse_args(argc, argv));
}
