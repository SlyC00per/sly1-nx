# SPDX-License-Identifier: AGPL-3.0-or-later
# The Switch smoke test: deko3d, uam-built shaders, romfs and an .nro, nothing of the game.
#   cmake -S tests/switch_smoke -B build-smoke -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake
#   cmake --build build-smoke
cmake_minimum_required(VERSION 3.16)
project(sly1_smoke C)

if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake")
endif()

add_executable(sly1_smoke main.c)
target_compile_options(sly1_smoke PRIVATE -Wall -Wextra -O2)
target_link_libraries(sly1_smoke PRIVATE deko3d)

nx_add_shader_program(smoke_vsh smoke_vsh.glsl vert)
nx_add_shader_program(smoke_fsh smoke_fsh.glsl frag)

dkp_add_asset_target(sly1_smoke_romfs ${CMAKE_CURRENT_BINARY_DIR}/romfs)
dkp_install_assets(sly1_smoke_romfs DESTINATION shaders TARGETS smoke_vsh smoke_fsh)

nx_generate_nacp(sly1_smoke.nacp NAME "sly1 smoke test" AUTHOR "sly1-nx" VERSION "0.0.1")
nx_create_nro(sly1_smoke NACP sly1_smoke.nacp ROMFS sly1_smoke_romfs)
