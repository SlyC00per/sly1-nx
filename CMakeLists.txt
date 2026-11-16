# SPDX-License-Identifier: AGPL-3.0-or-later
# The port's build: one tree, two targets.
#   Host (the main test loop, AddressSanitizer and UBSan):
#     cmake -B build-host -G Ninja -DPLATFORM=HOST_ASAN && cmake --build build-host && ctest --test-dir build-host
#   Switch (devkitPro):
#     cmake -B build-switch -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake && cmake --build build-switch
cmake_minimum_required(VERSION 3.16)
# On an Apple Silicon Mac, build for it (an Intel CMake under Rosetta would build for x86-64);
# the Switch is AArch64 too.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_TOOLCHAIN_FILE)
    execute_process(COMMAND sysctl -n hw.optional.arm64 OUTPUT_VARIABLE SLY1_ARM64 ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(SLY1_ARM64 STREQUAL "1")
        set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "")
    endif()
endif()
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
# Frame records on every target, for the port's reports of where a fault came from.
add_compile_options(-fno-omit-frame-pointer)

if(PLATFORM STREQUAL "HOST_ASAN")
    # No stack-use-after-return detection: it moves locals to a heap of its own, and the game's
    # locals must stay on its stacks in game memory (port/ptr32.h).
    set(SANITIZE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
        -fsanitize-address-use-after-return=never)
    add_compile_options(${SANITIZE} -g)
    add_link_options(${SANITIZE})
endif()

# The port's own code: game memory and, later, the platform layer.
add_library(port_core STATIC port/platform/game_memory.cpp port/platform/vu0_micro.cpp port/platform/varargs.cpp
    port/platform/system.cpp
    port/platform/memory.cpp port/platform/thread.cpp)
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

# --- The game: sly1-decomp's code, written again for the port by tools/gen.py (docs/DESIGN.md,
# section 3), into the build folder. The submodule is never changed. Needs Python with the libclang
# package (venv/ at the root, or the system's); CI builds without it for now (SLY1_BUILD_GAME=OFF).
option(SLY1_BUILD_GAME "Build the game (needs Python with libclang)" ON)
if(SLY1_BUILD_GAME)
set(SLY1_DECOMP ${CMAKE_SOURCE_DIR}/sly1-decomp)
set(GEN_DIR ${CMAKE_BINARY_DIR}/gen)
if(EXISTS ${CMAKE_SOURCE_DIR}/venv/bin/python)
    set(SLY1_PYTHON ${CMAKE_SOURCE_DIR}/venv/bin/python)
elseif(EXISTS ${CMAKE_SOURCE_DIR}/venv/Scripts/python.exe)
    set(SLY1_PYTHON ${CMAKE_SOURCE_DIR}/venv/Scripts/python.exe)
else()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(SLY1_PYTHON ${Python3_EXECUTABLE})   # needs the libclang package (pip install libclang)
endif()

file(GLOB DECOMP_UNITS CONFIGURE_DEPENDS ${SLY1_DECOMP}/src/main/*.cpp)
file(GLOB_RECURSE DECOMP_INPUTS CONFIGURE_DEPENDS ${SLY1_DECOMP}/include/*.h ${SLY1_DECOMP}/src/main/*.cpp)
file(GLOB GEN_TOOLS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen*.py ${CMAKE_SOURCE_DIR}/tools/patches.py
     ${CMAKE_SOURCE_DIR}/port/include/port/ps2/*.h)
set(GEN_UNITS ${GEN_DIR}/port_data_init.cpp ${GEN_DIR}/src/lib/sort.cpp)
foreach(unit ${DECOMP_UNITS})
    get_filename_component(name ${unit} NAME)
    list(APPEND GEN_UNITS ${GEN_DIR}/src/main/${name})
endforeach()
add_custom_command(OUTPUT ${GEN_UNITS}
    COMMAND ${SLY1_PYTHON} ${CMAKE_SOURCE_DIR}/tools/gen.py --out ${GEN_DIR} --all
    DEPENDS ${DECOMP_INPUTS} ${GEN_TOOLS} ${SLY1_DECOMP}/config/us/c_data.tsv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Writing sly1-decomp's game code for the port (tools/gen.py)")
add_custom_target(sly1_gen DEPENDS ${GEN_UNITS})

# The flags of every unit that sees the game's headers.
# (SHELL: keeps each -include with its header: CMake removes repeated options otherwise.)
# -fwrapv: signed arithmetic wraps, as the PS2's does (the game's hashes multiply into overflow).
# -fno-delete-null-pointer-checks: two methods test `this == 0` and are called on null pointers,
# which g++ 2.95 compiled as written.
# -fno-strict-aliasing: the game reads one object through pointers of other types throughout (a
# word as a float, a record as bytes), which g++ 2.95 compiled as written; newer compilers (GCC most
# of all, the Switch's) would reorder or drop such accesses.
# -fsigned-char: plain char is signed, as the PS2's g++ had it; AArch64's ABI (the Switch's)
# makes it unsigned, where Apple's and x86-64's keep it signed.
set(GAME_FLAGS -w -ffp-contract=off -fwrapv -fno-strict-aliasing -fsigned-char -fno-delete-null-pointer-checks "SHELL:-include port/gxx_ext.h" "SHELL:-include port/ptr32.h"
    "SHELL:-include port/varargs.h" "SHELL:-include port/retail_data.h" -Wno-register)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND GAME_FLAGS -Wno-c++11-narrowing)
else()
    list(APPEND GAME_FLAGS -Wno-narrowing)
endif()
set(GAME_INCLUDES ${CMAKE_SOURCE_DIR}/port/include ${GEN_DIR}/include ${GEN_DIR}/src)

add_library(sly1_game OBJECT ${GEN_UNITS})
option(SLY1_SENTINEL "Check a word after the thread stacks at every preemption point" OFF)
if(SLY1_SENTINEL)
    add_compile_definitions(PORT_SENTINEL)
endif()
target_compile_options(sly1_game PRIVATE ${GAME_FLAGS})
if(PLATFORM STREQUAL "HOST_ASAN")
    # The game indexes past a member array on purpose (views that start before an array or run on
    # into the next member, inside the same object, in retail's layout): UBSan's declared-bounds
    # check stays on for the port's own code; AddressSanitizer still catches any access outside an
    # object. Its list walks also step a cursor past the last element, null plus the link's offset,
    # which is never read: the pointer-overflow check is off for game code too. So is the shift of
    # a negative value (two's complement, as on the PS2 and in C++20); shifts by too much stay
    # checked, since MIPS and ARM treat them differently.
    # And the null check: those methods are called on null on purpose; a real null dereference
    # still faults, which AddressSanitizer reports. And alignment: the game views retail's data
    # through types of its own (an object's descriptor read as a PlacedObject, 16-byte aligned),
    # plain loads that ARM64 and x86-64 do at any alignment. And float-cast-overflow: a float too
    # large for an int saturates on the PS2 (cvt.w.s) as on ARM64 (fcvtzs), the Switch's.
    target_compile_options(sly1_game PRIVATE
        -fno-sanitize=bounds,pointer-overflow,shift-base,null,alignment,float-cast-overflow)
    # The room audit (tools/gen_pointers.py, port/retail_data.h): reports every access to the bytes
    # given to locals past their own size, and goes on.
    option(SLY1_ROOM_AUDIT "Report accesses to the room given to game locals" OFF)
    if(SLY1_ROOM_AUDIT)
        target_compile_definitions(sly1_game PRIVATE PORT_ROOM_AUDIT)
        target_compile_options(sly1_game PRIVATE -fsanitize-recover=address)
    endif()
endif()
target_include_directories(sly1_game PRIVATE ${GAME_INCLUDES})

# The platform layer: what replaces sly1-decomp's src/lib and src/libm.
# The game's math library: sly1-decomp's own C (src/libm, matched to retail), compiled as it is, so
# the game's results are retail's to the bit rather than the host library's.
file(GLOB LIBM_SOURCES CONFIGURE_DEPENDS ${SLY1_DECOMP}/src/libm/*.c)
add_library(port_libm STATIC ${LIBM_SOURCES})
target_include_directories(port_libm PRIVATE ${SLY1_DECOMP}/include)
target_compile_options(port_libm PRIVATE -w -ffp-contract=off -fwrapv -fno-strict-aliasing -fsigned-char)

add_library(port_platform STATIC
    port/platform/lib_runtime.cpp port/platform/lib_kernel.cpp port/platform/lib_devices.cpp
    port/platform/iop.cpp port/platform/iso.cpp port/platform/float_mode.cpp
    port/platform/kernel.cpp port/platform/lib_services.cpp port/platform/dmac.cpp
    port/platform/memory_card.cpp port/platform/pad.cpp)
target_compile_options(port_platform PRIVATE ${GAME_FLAGS})
target_include_directories(port_platform PRIVATE ${GAME_INCLUDES})
add_dependencies(port_platform sly1_gen)

# The renderer: VU1 (port/render/vu1.cpp) and its microcode translated to C++ from the player's
# disc (tools/vu1_translate.py, written into the build folder only). Without -DSLY1_ISO the table
# of translated chunks is empty and the interpreter runs everything.
set(SLY1_ISO "" CACHE FILEPATH "The player's disc image, for the VU1 microcode's translation")
if(SLY1_ISO)
    set(VU1_ARGS --iso ${SLY1_ISO})
else()
    set(VU1_ARGS --empty)
endif()
add_custom_command(OUTPUT ${GEN_DIR}/vu1_programs.cpp
    COMMAND ${SLY1_PYTHON} ${CMAKE_SOURCE_DIR}/tools/vu1_translate.py ${VU1_ARGS} --out ${GEN_DIR}/vu1_programs.cpp
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/vu1_translate.py ${CMAKE_SOURCE_DIR}/tools/level_bytes.py
    COMMENT "Translating the game's VU1 microcode (tools/vu1_translate.py)"
    VERBATIM)
add_library(port_render STATIC port/render/vu1.cpp port/render/gs.cpp ${GEN_DIR}/vu1_programs.cpp)
target_include_directories(port_render PRIVATE port/include port/render)
target_compile_options(port_render PRIVATE -Wall -Wextra -Wno-unused-label -Wno-unused-variable)
# The translated microcode, generated from the core the interpreter runs: optimised and without
# the sanitizers' checks in every build (instrumented, its inlined instructions came to 290 MB).
set_source_files_properties(${GEN_DIR}/vu1_programs.cpp PROPERTIES COMPILE_OPTIONS "-O2;-fno-sanitize=all")

# The game's hand-written assembly functions, in C++ (port/game_asm), with the game's headers.
add_library(port_game_asm STATIC port/game_asm/game_asm.cpp)
target_compile_options(port_game_asm PRIVATE ${GAME_FLAGS})
target_include_directories(port_game_asm PRIVATE ${GAME_INCLUDES})
add_dependencies(port_game_asm sly1_gen)

if(PLATFORM STREQUAL "SWITCH")
    add_executable(sly1 port/host/main.cpp $<TARGET_OBJECTS:sly1_game>)
    target_include_directories(sly1 PRIVATE port/include)
    target_link_libraries(sly1 PRIVATE port_game_asm port_platform port_render port_libm port_core)
    nx_generate_nacp(sly1.nacp NAME "Sly Cooper" AUTHOR "sly1-nx" VERSION "0.0.1")
    nx_create_nro(sly1 NACP sly1.nacp)
else()
    find_package(Threads REQUIRED)
    add_executable(sly1_headless port/host/main.cpp $<TARGET_OBJECTS:sly1_game>)
    target_include_directories(sly1_headless PRIVATE port/include)
    target_link_libraries(sly1_headless PRIVATE port_game_asm port_platform port_render port_libm port_core Threads::Threads)
endif()
endif()  # SLY1_BUILD_GAME
