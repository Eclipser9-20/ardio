cmake_minimum_required(VERSION 3.20)
project(ardio VERSION 0.1.0 LANGUAGES CXX)

# Lets editors and clangd resolve includes without guessing.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Werror)

# A system-wide compiler shim may inject -L flags into compile-only steps, where
# they are unused. ardio has no third-party dependencies, so that is harmless --
# but -Werror would make it fatal.
add_compile_options(-Wno-unused-command-line-argument)

# ---------------------------------------------------------------- libardio ---

add_library(libardio STATIC
    src/hex.cpp
    src/board.cpp
    src/monitor.cpp
    src/protocol/stk500v1.cpp
    src/protocol/programmer.cpp
    src/platform/ports_select.cpp
    src/toolchain/config.cpp
    src/toolchain/search.cpp
    src/toolchain/fetch.cpp
    src/toolchain/build_toolchain.cpp
    src/avr/assembler.cpp
    src/avr/lexer.cpp
    src/avr/preprocess.cpp
    src/build/compile.cpp
    src/cli/args.cpp
    src/cli/commands.cpp
)
set_target_properties(libardio PROPERTIES OUTPUT_NAME ardio)
target_include_directories(libardio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Platform layer. macOS is implemented; other platforms build against a stub
# that reports the limitation rather than failing to link.
if(APPLE)
    target_sources(libardio PRIVATE
        src/platform/macos/serial_macos.cpp
        src/platform/macos/ports_macos.cpp
    )
    target_link_libraries(libardio PUBLIC "-framework IOKit" "-framework CoreFoundation")
else()
    message(WARNING
        "ardio's platform layer is implemented for macOS only so far. "
        "Building with stubs: port enumeration and serial I/O will report "
        "that this platform is not yet supported.")
    target_sources(libardio PRIVATE src/platform/unsupported.cpp)
endif()

# --------------------------------------------------------------------- CLI ---

add_executable(ardio src/cli/main.cpp)
target_link_libraries(ardio PRIVATE libardio)

# ------------------------------------------------------------------- tests ---

add_executable(ardio_tests
    tests/main.cpp
    tests/test_harness.cpp
    tests/test_hex.cpp
    tests/test_board.cpp
    tests/test_stk500v1.cpp
    tests/test_serial_fake.cpp
    tests/test_programmer.cpp
    tests/test_ports.cpp
    tests/test_config.cpp
    tests/test_build.cpp
    tests/test_cli_args.cpp
    tests/test_fetch.cpp
    tests/test_toolchain_build.cpp
    tests/test_avr_assembler.cpp
    tests/test_lexer.cpp
    tests/test_preprocess.cpp
    tests/test_runtime_core.cpp
)
target_link_libraries(ardio_tests PRIVATE libardio)
target_include_directories(ardio_tests PRIVATE tests)

enable_testing()
add_test(NAME unit COMMAND ardio_tests)

# ----------------------------------------------------------------- install ---

include(GNUInstallDirs)
install(TARGETS ardio RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS libardio ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ardio DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
