# SPDX-License-Identifier: AGPL-3.0-or-later
# The port's build: one tree, two targets.
#   Host (the main test loop, AddressSanitizer and UBSan):
#     cmake -B build-host -G Ninja -DPLATFORM=HOST_ASAN && cmake --build build-host && ctest --test-dir build-host
#   Switch (devkitPro):
#     cmake -B build-switch -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake && cmake --build build-switch
cmake_minimum_required(VERSION 3.16)
project(sly1_nx C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NINTENDO_SWITCH)
    set(PLATFORM SWITCH)
else()
    set(PLATFORM HOST_ASAN CACHE STRING "Host build: HOST_ASAN (sanitizers) or HOST (plain)")
    set_property(CACHE PLATFORM PROPERTY STRINGS HOST_ASAN HOST)
    enable_testing()
endif()
message(STATUS "sly1-nx: platform ${PLATFORM}")

if(PLATFORM STREQUAL "HOST_ASAN")
    set(SANITIZE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_compile_options(${SANITIZE} -g)
    add_link_options(${SANITIZE})
endif()

# The port's own code: game memory and, later, the platform layer.
add_library(port_core STATIC port/platform/game_memory.cpp port/platform/varargs.cpp
    port/platform/system.cpp
    port/platform/memory.cpp)
target_include_directories(port_core PUBLIC port/include)
target_compile_options(port_core PRIVATE -Wall -Wextra)

# Unit tests that need no disc: on the host through ctest, on the Switch as an .nro that
# tools/run_ryujinx.py runs.
set(UNIT_SOURCES tests/unit/unit.cpp tests/unit/test_ptr32.cpp tests/unit/test_vu0.cpp)
if(PLATFORM STREQUAL "SWITCH")
    add_executable(sly1_unit ${UNIT_SOURCES} tests/unit/main_switch.cpp)
    target_link_libraries(sly1_unit PRIVATE port_core)
    nx_generate_nacp(sly1_unit.nacp NAME "sly1 unit tests" AUTHOR "sly1-nx" VERSION "0.0.1")
    nx_create_nro(sly1_unit NACP sly1_unit.nacp)
else()
    add_executable(sly1_unit ${UNIT_SOURCES} tests/unit/main_host.cpp)
    target_link_libraries(sly1_unit PRIVATE port_core)
    add_test(NAME unit COMMAND sly1_unit)
endif()
target_compile_options(sly1_unit PRIVATE -Wall -Wextra)
