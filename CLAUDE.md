# Sly Cooper port — Nintendo Switch (AArch64, libnx, deko3d)

This file is the port repository's root `CLAUDE.md`. The decompilation is the submodule
`sly1-decomp/`, which has its own `CLAUDE.md` and `AGENTS.md` for its own rules.

## What this is

A native Switch homebrew port of *Sly Cooper and the Thievius Raccoonus* (PS2, NTSC-U
`SCUS_971.98`), built from the finished matching decompilation in the submodule `sly1-decomp/`
(pinned; update it only by a deliberate submodule bump). **It matches the PS3 release as
closely as possible** (`docs/PS3_PARITY.md`): 16:9 widescreen (Hor+), HD textures, no memory
card, HD output at a locked 60 FPS; plus low power use.

## Rules

1. **The decompilation is read-only.** Never edit `sly1-decomp/`. Its sources are compiled
   as they are; the port supplies replacement headers, a library layer and a source rewrite
   step. If the decomp needs a fix, stop and tell the user.
2. **No proprietary material in git:** no disc images, executables, extracted assets, HD
   textures, compilers or SDK files. Assets are read from the user's `.iso` at run time.
3. **Derive from the decomp and its docs** (`sly1-decomp/docs/PORTING.md`, `DISC.md`,
   `OBJECTS.md`). Nothing from Sony's SDK, the game's prototype, or other Sly projects. OpenRAC
   code may be reused, credited (OpenThievius's `docs/policy/SOURCING.md`).
4. **One milestone at a time:** build, run its verification, commit, summarise, then continue.
   Report what was actually run; say plainly what could not be run.
5. **A behaviour difference from retail is a port bug,** unless `docs/PS3_PARITY.md` lists it.
   The decompiled code is byte-exact; do not "tidy" game logic, `goto`s or statement order.
6. **Match the PS3 version as closely as possible** (maintainer, 2026-11-14): widescreen, HD
   textures, no memory cards, and every other difference `docs/PS3_PARITY.md` lists. The PS3
   release is a reference for behaviour, observed on the maintainer's own copy: never copy its
   code, and never implement a PS3 behaviour from memory; record what was observed first.

## Facts that are easy to get wrong

- No `.WAD` files, no file names: data is found by sector through records in the executable.
  Levels are LZSS streams read field by field by the game's own loaders.
- No Sony header or function names in the source: the library layer is `src/lib/` under the
  project's own names.
- LP64: pointers are 8 bytes here, 4 on the PS2. Integers that hold addresses are `sptr`/`uptr`.
  Never cast a pointer to `int`/`u32`/`long`. Structure `check_` assertions, class sizes in the
  descriptors and `count * 4` allocations assume 4-byte pointers. Script values hold handles in
  32 bits.
- g++ 2.95 extensions remain in the decomp by design: named return values (nine functions),
  `<?` `>?` `>?=`, and the old compiler's varargs and exception internals. `tools/modern_check.py`
  there lists and rewrites them.
- Vector math is the `include/vu0.h` seam; each helper's "Computes:" line is its specification.
  Keep the PS2 float conventions it documents where gameplay depends on them.
- Colour and alpha: 0x80 is 1.0. Textures are 4/8-bit indices with CLUTs in CSM1 order, or
  32-bit. Read depth, alpha-test and blend behaviour from the GS state the game sets, not from
  assumptions.
- Sound is commands to a driver the port must impersonate (bank load, start by bank and record,
  adjust, stop); the game waits on replies. Videos are MPEG program streams.

## Commands

Fill these in as the build takes shape; keep them current. They must work on macOS, Windows
(MSYS2 or Visual Studio) and Linux: devkitPro tools through `$DEVKITPRO`, never a hard-coded path.

- Host (ASan/UBSan), the main test loop:
  `cmake -B build-host -DPLATFORM=HOST_ASAN -G Ninja && cmake --build build-host && build-host/sly1_headless --iso <path> --verify-levels`
- Every level in turn (load, then 60 frames): `build-host/sly1_headless --iso <path> --verify-levels [--from-level N]`.
- Headless run for N seconds: `build-host/sly1_headless --iso <path> --seconds N`;
  it ends with the kernel's threads, the DMA counts and the frame semaphores. `SLY1_DMAC_TRACE=1`
  prints every DMA chain; `UBSAN_OPTIONS=print_stacktrace=1` gives UBSan reports a stack.
  Saves go to `--save FOLDER` (default `save/`, gitignored); `SLY1_CARD_TRACE=1` prints every
  memory card request.
- Switch: `cmake -B build-switch -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake -G Ninja && cmake --build build-switch`
- Shaders: `$DEVKITPRO/tools/bin/uam -s <vert|frag> <in> -o <out.dksh>` (run by the build)
- Run in the emulator (Ryujinx), unattended: `python3 tools/run_ryujinx.py build-switch/sly1.nro`
  (headless, its own data folder `build-ryujinx/`; prints the program's debug-output lines, stops
  at "<name>: done, status N" and exits with N; copies `sdmc:/sly1/` next to the .nro, frames as
  PNG; `--docked` for 1920x1080). Ryujinx from `--ryujinx`, `$SLY1_RYUJINX` or the usual places.
  Homebrew needs no firmware or keys.
- Switch smoke test (toolchain and emulator, no game code): `cmake -S tests/switch_smoke -B build-smoke -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake && cmake --build build-smoke && python3 tools/run_ryujinx.py build-smoke/sly1_smoke.nro`
- Run on the Switch Lite (handheld only): `$DEVKITPRO/tools/bin/nxlink -s build-switch/sly1.nro`
  with hbmenu's netloader open.
- Symbolicate a crash: `$DEVKITPRO/devkitA64/bin/aarch64-none-elf-addr2line -e build-switch/sly1.elf -f -C -p <offset>`
- The decomp's own checks (must stay green, run in `sly1-decomp/`): `python3 tools/build.py` prints
  `MATCH: build/us/SCUS_971.98 sha1 57dc305d…`; `python3 tools/modern_check.py --check`.
- After any rendering change, dump a frame (`--frames N --dump-fb out/frame.png`) and look at it.
