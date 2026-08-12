cmake_minimum_required(VERSION 3.20)
project(ardio CXX)

# Lets editors and clangd resolve includes without guessing.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

# A system-wide clang++ shim may inject -L flags (e.g. Homebrew's lib tree) into
# compile-only steps, where they are unused. ardio has no third-party
# dependencies, so that is harmless -- but -Werror would make it fatal.
add_compile_options(-Wno-unused-command-line-argument)

add_library(libardio STATIC)
set_target_properties(libardio PROPERTIES OUTPUT_NAME ardio)
target_include_directories(libardio PUBLIC include)
target_sources(libardio PRIVATE src/hex.cpp src/board.cpp src/protocol/stk500v1.cpp)

if(APPLE)
    target_sources(libardio PRIVATE src/platform/macos/serial_macos.cpp)
    target_link_libraries(libardio PUBLIC "-framework IOKit" "-framework CoreFoundation")
endif()

add_executable(ardio_tests tests/main.cpp tests/test_harness.cpp tests/test_hex.cpp tests/test_board.cpp tests/test_stk500v1.cpp tests/test_serial_fake.cpp)
target_link_libraries(ardio_tests PRIVATE libardio)
target_include_directories(ardio_tests PRIVATE tests)

enable_testing()
add_test(NAME unit COMMAND ardio_tests)
