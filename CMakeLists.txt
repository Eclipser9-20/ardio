cmake_minimum_required(VERSION 3.20)
project(ardio VERSION 0.1.0 LANGUAGES C CXX)

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

# ------------------------------------------------------------------ libhike ---
#
# A library in its own right, not an internal detail of ardio. It is useful to
# anything that wants a terminal UI, so it is built and installed separately
# and ardio simply links it -- which also keeps the dependency one-way and
# stops ardio's types leaking into it.

add_library(hike STATIC
    libhike/src/hike_buffer.c
    libhike/src/hike_term.c
    libhike/src/hike_input.c
    libhike/src/hike_unicode.c
    libhike/src/hike_widgets.c
    libhike/src/hike_cpp.cpp
)

set_source_files_properties(
    libhike/src/hike_buffer.c libhike/src/hike_term.c libhike/src/hike_input.c
    libhike/src/hike_unicode.c libhike/src/hike_widgets.c
    PROPERTIES LANGUAGE C)

target_include_directories(hike PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libhike/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(hike PROPERTIES OUTPUT_NAME hike)

# ---------------------------------------------------------------- libardio ---

add_library(libardio STATIC
    src/hex.cpp
    src/board.cpp
    src/wifi_config.cpp
    src/monitor.cpp
    src/protocol/stk500v1.cpp
    src/protocol/programmer.cpp
    src/protocol/esp_rom.cpp
    src/protocol/stk500v2.cpp
    src/protocol/avr109.cpp
    src/platform/ports_select.cpp
    src/platform/remote_serial.cpp
    src/toolchain/config.cpp
    src/toolchain/search.cpp
    src/toolchain/fetch.cpp
    src/toolchain/build_toolchain.cpp
    src/avr/assembler.cpp
    src/avr/device.cpp
    src/emu/core_reference.cpp
    src/emu/core_arm64.cpp
    src/emu/core_select.cpp
    src/emu/board.cpp
    src/emu/parts.cpp
    src/tui/emulate_view.cpp
    src/tui/emulate_app.cpp
    src/emu/peripherals.cpp
    src/avr/lexer.cpp
    src/avr/preprocess.cpp
    src/avr/parser.cpp
    src/avr/type.cpp
    src/avr/sema.cpp
    src/avr/codegen_expr.cpp
    src/avr/peephole.cpp
    src/avr/sketch.cpp
    src/avr/codegen_stmt.cpp
    src/avr/codegen_class.cpp
    src/avr/compiler.cpp
    src/build/compile.cpp
    src/cli/args.cpp
    src/cli/commands.cpp
)
set_target_properties(libardio PROPERTIES OUTPUT_NAME ardio)
target_include_directories(libardio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libhike/include>
    $<INSTALL_INTERFACE:include>
)



# Platform layer. macOS is implemented; other platforms build against a stub
# that reports the limitation rather than failing to link.
if(APPLE)
    target_sources(libardio PRIVATE
        src/platform/macos/serial_macos.cpp
        src/platform/macos/ports_macos.cpp
        src/platform/macos/usb_serial_macos.cpp
    )
    target_link_libraries(libardio PUBLIC "-framework IOKit" "-framework CoreFoundation")
endif()

target_link_libraries(libardio PUBLIC hike)

if(APPLE)
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
    tests/test_wifi_config.cpp
    tests/test_board.cpp
    tests/test_stk500v1.cpp
    tests/test_stk500v2.cpp
    tests/test_avr109.cpp
    tests/test_device.cpp
    tests/test_emu_core.cpp
    tests/test_emu_jit.cpp
    tests/test_emu_board.cpp
    tests/test_emu_parts.cpp
    libhike/tests/test_hike_buffer.cpp
    libhike/tests/test_hike_input.cpp
    libhike/tests/test_hike_widgets.cpp
    tests/test_tui_view.cpp
    tests/test_tui_app.cpp
    tests/test_emu_peripherals.cpp
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
    tests/test_include.cpp
    tests/test_runtime_core.cpp
    tests/test_parser.cpp
    tests/test_type.cpp
    tests/test_sema.cpp
    tests/test_codegen_expr.cpp
    tests/test_codegen_frame.cpp
    tests/test_runtime_motors.cpp
    tests/test_runtime_io.cpp
    tests/test_sketch.cpp
    tests/test_compiler.cpp
    tests/test_ir_turret.cpp
    tests/test_label_maker.cpp
    tests/test_aggregate_init.cpp
    tests/test_original_label_maker.cpp
    tests/test_runtime_string.cpp
    tests/test_codegen_class.cpp
    tests/test_runtime_headers.cpp
    tests/test_runtime_ir.cpp
    tests/test_runtime_serial.cpp
    tests/test_lightweight.cpp
    tests/test_peephole.cpp
    tests/test_flash_data.cpp
    tests/test_esp_rom.cpp
)
target_link_libraries(ardio_tests PRIVATE libardio)
target_include_directories(ardio_tests PRIVATE tests)
target_compile_definitions(ardio_tests PRIVATE ARDIO_TEST_TMP="${CMAKE_BINARY_DIR}")

enable_testing()
add_test(NAME unit COMMAND ardio_tests)

# ----------------------------------------------------------------- install ---

include(GNUInstallDirs)
install(TARGETS ardio RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The AVR runtime has to ship too. find_runtime_dir looks here once ardio is
# installed, and without it an installed ardio can flash and emulate but
# cannot build a sketch -- it would report every runtime symbol as undefined.
install(DIRECTORY runtime/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/ardio/runtime)
install(TARGETS libardio EXPORT ardioTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS hike EXPORT ardioTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ardio DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY libhike/include/hike DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# --- package discovery --------------------------------------------------
#
# Without these, a project that installs ardio still cannot use it:
# find_package(ardio) finds nothing, and an editor's language server has no
# idea where <ardio/...> or <hike/...> live, so every include is underlined in
# red even though the compiler would resolve it. Exporting targets fixes both,
# because the same files tell CMake and any tool reading its output.

include(CMakePackageConfigHelpers)

install(EXPORT ardioTargets
        FILE ardioTargets.cmake
        NAMESPACE ardio::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ardio)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ardioConfig.cmake
"include(\"\${CMAKE_CURRENT_LIST_DIR}/ardioTargets.cmake\")\n")

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ardioConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ardioConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ardioConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ardio)

# libhike is usable on its own, so it gets its own package name too. A project
# that only wants a terminal UI should not have to find_package(ardio).
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/hikeConfig.cmake
"include(\"\${CMAKE_CURRENT_LIST_DIR}/../ardio/ardioTargets.cmake\")\n")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hikeConfig.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hike)

# pkg-config, for build systems that are not CMake.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ardio.pc
"prefix=${CMAKE_INSTALL_PREFIX}\n"
"includedir=\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}\n"
"libdir=\${prefix}/${CMAKE_INSTALL_LIBDIR}\n"
"\n"
"Name: ardio\n"
"Description: Build, flash, monitor and emulate Arduino boards\n"
"Version: ${PROJECT_VERSION}\n"
"Cflags: -I\${includedir}\n"
"Libs: -L\${libdir} -lardio -lhike\n")

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/hike.pc
"prefix=${CMAKE_INSTALL_PREFIX}\n"
"includedir=\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}\n"
"libdir=\${prefix}/${CMAKE_INSTALL_LIBDIR}\n"
"\n"
"Name: hike\n"
"Description: Cross-platform terminal user interface library\n"
"Version: ${PROJECT_VERSION}\n"
"Cflags: -I\${includedir}\n"
"Libs: -L\${libdir} -lhike\n")

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ardio.pc
              ${CMAKE_CURRENT_BINARY_DIR}/hike.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# --- editor discovery ---------------------------------------------------
#
# clangd resolves includes from the compiler's flags, so a file that is not
# part of a project with a compile_commands.json gets no include path at all
# and every <ardio/...> or <hike/...> line is underlined even though the
# compiler would find it. Adding the install prefix to the global config fixes
# that everywhere at once.
#
# This edits a file outside the install prefix, which an install should
# normally never do, so it is written to be safe about it: it only ever
# appends, it does nothing if the path is already present, and it leaves any
# existing configuration untouched.
install(CODE "
    set(clangd_dir \"\$ENV{HOME}/.config/clangd\")
    set(clangd_config \"\${clangd_dir}/config.yaml\")
    set(include_flag \"-I${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}\")

    set(existing \"\")
    if(EXISTS \"\${clangd_config}\")
        file(READ \"\${clangd_config}\" existing)
    endif()

    if(existing MATCHES \"\${include_flag}\")
        message(STATUS \"clangd already knows \${include_flag}\")
    elseif(existing STREQUAL \"\")
        file(MAKE_DIRECTORY \"\${clangd_dir}\")
        file(WRITE \"\${clangd_config}\"
\"CompileFlags:
  Add:
    # ardio and libhike install their headers here.
    - \${include_flag}
\")
        message(STATUS \"Wrote \${clangd_config}\")
    elseif(existing MATCHES \"CompileFlags:\" AND existing MATCHES \"Add:\")
        string(REGEX REPLACE \"(Add:[ \\t]*\\n)\"
               \"\\\\1    # ardio and libhike install their headers here.\\n    - \${include_flag}\\n\"
               patched \"\${existing}\")
        file(WRITE \"\${clangd_config}\" \"\${patched}\")
        message(STATUS \"Added \${include_flag} to \${clangd_config}\")
    else()
        # A config exists but is not in a shape we can safely edit, so say what
        # to add rather than rewriting someone's file badly.
        message(WARNING
            \"Could not update \${clangd_config} automatically. \"
            \"Add this under CompileFlags/Add:  \${include_flag}\")
    endif()
")
