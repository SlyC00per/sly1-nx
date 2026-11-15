# Design note

Status: **proposed**, waiting for the maintainer's go-ahead. Nothing here is built yet. Where
a claim was measured, the command's result is given; where it is a plan or an assumption, it
says so. Written on 2026-11-14 against `sly1-decomp` at `8f7a85a3` (its `master` that day).

## 1. What is there

**This repository** holds `CLAUDE.md`, `docs/BRIEF.md`, the licence, a README and the
submodule. No code yet.

**The submodule** (`sly1-decomp/`), counted with `ls`, `wc` and `grep`:

| Part | Size | What the port does with it |
| --- | --- | --- |
| `src/main/` | 101 files, 134,958 lines | compiled, through the generation step of section 3 |
| `src/lib/` | 61 files, 12,846 lines | replaced by the port's platform layer (section 5) |
| `src/libm/` | 35 files | not built: the platform's `libm` |
| `src/versions/eu/` | PAL's own definitions | not built (the target is NTSC-U) |
| `include/` | 206 headers | used as they are, except `vu0.h`, `fpu.h`, `system.h`, `mul_wide.h`, `asm_func.h`, which the port replaces |
| `config/names/`, `include/names.h` | 6,243 names | used as they are: readable name and address name are one symbol |
| original assembly in game code | 48 `ASM_FUNC`, 112 linker leftovers | the 48 written in the port from their roles; the leftovers dropped |
| `.vutext` | 0x5550 bytes of VU0 and VU1 microcode | not run; its work is redone in the two VU0 callers and in the renderer |

What the layouts look like, because the 64-bit decision turns on it:

- 1,453 structure definitions: 610 in `include/`, **843 local to a source file**, many of
  them a file's own view of a class another file also describes;
- **1,109 padding arrays sized by absolute offsets** (`u8 pad2D4[0x2E0 - 0x2D4];`);
- 301 `check_` size assertions, 5,596 fields with their offset in a comment, 65 raw-offset
  accesses, 319 vector members that need 16-byte alignment, about 1,100 pointer members;
- 240 derived structures, no member-function pointers, `virtual` only in PAL's files.

## 2. What I can and cannot run here

Checked on this machine (macOS 14.5, Apple Silicon):

| | |
| --- | --- |
| Host build | **yes**: Apple Clang 15, CMake 3.29.3, Ninja; ASan and UBSan |
| Switch build | **yes, to an `.nro`**: `$DEVKITPRO=/opt/devkitpro` has devkitA64, libnx, deko3d, `uam`, `nxlink`, `Switch.cmake` |
| The disc | **yes**: the NTSC-U image in the decompilation checkout's `baserom/`, passed with `--iso` |
| Running the `.nro` in an emulator | **yes**, since 2026-11-14: Ryujinx 1.3.353 headless through `tools/run_ryujinx.py`; `tests/switch_smoke` draws with deko3d, reads the frame back and writes it to the emulated SD card (12.5 s a run). An emulator is not the hardware: what only Ryujinx has shown is reported as such |
| Running on the Switch Lite | **only with you**: I can send a build with `nxlink -s` and read its log; I cannot see the screen, hear it or press buttons |
| The matching build in the submodule | **not tried yet**: `SLY1_TOOLCHAIN` is not set in this shell and the submodule has no `baserom/`; wine is installed. I will set it up before milestone 1 and report what `tools/build.py` prints |
| libclang for the generation step | not installed; `pip install libclang` in a venv (a build tool, not linked into the port) |
| Windows and Linux | only through CI |

Consequence: everything up to the renderer's draw list I can verify myself on the host, and
deko3d's output in Ryujinx. What the real hardware puts on its screen, and sound by ear, I
cannot.

**Found in the first emulator run, to check on hardware:** with deko3d's default device flags
(`OriginUpperLeft`, depth 0 to 1), Ryujinx puts clip-space y = +1 at the top of the window (a
vertex at y = -0.8 lands near the bottom), while window row 0 is the top (a scissored clear at
rows 0-99 lands at the top of the read-back). The renderer must not rely on the sign of clip
space y until the smoke test has been run on the Switch Lite and agrees. Section 7 proposes a host
reference backend so that frames can still be dumped and looked at here; anything checked
only that way will be reported as "host reference only, not seen on a Switch".

## 3. How the decompiled sources are built

### Recommendation on the two open decisions

1. **Through a generation step, not directly.** A small tool reads the decompilation's
   sources and writes a rewritten copy under `build-*/gen/`; the compiler only ever sees the
   copy. The submodule is never edited. Reasons: three things cannot be handled by include
   paths alone (g++ 2.95's named return values and `<?` `>?` operators, which
   `tools/modern_check.py` already rewrites on a copy; the pointer model below; the float
   rules of section 6), and a generated tree keeps every such change mechanical, repeatable
   and reviewable as a diff against the submodule.
2. **C++ with CMake and Ninja.** The port's own code in C++20; the generated game code
   compiled as GNU C++ (the dialect settled by the spike in section 9); tools in Python 3.
   This is what the brief's constraints and OpenRAC's port already use, and the game is C++.

### The steps of a build

1. `tools/gen.py` (Python, libclang) copies `sly1-decomp/src/main` and `include` into
   `build-*/gen/`, applying: `modern_check.py`'s two rewrites (reused, credited), the
   pointer rewrite (section 4), the float rewrite (section 6), and a table of named,
   commented patches for anything else (each patch states the file, the reason and the
   retail behaviour it keeps). It runs as a CMake custom command with the submodule's
   files as dependencies, so a submodule bump regenerates.
2. Include paths put `port/include/` before the generated `include/`: the port's `vu0.h`,
   `fpu.h`, `system.h`, `mul_wide.h` and `asm_func.h` take the place of the decompilation's
   by name. `ASM_FUNC(x)` and `LINKER_REMNANT(x)` expand to nothing; the 48 hand-written
   functions get C++ in `port/game_asm/`, each written from its role in `config/names/` and
   its instructions in the disassembly of my own setup.
3. `port/platform/` defines the library functions the game calls, under the decompilation's
   own symbols (section 5).
4. Two targets from one tree: `-DPLATFORM=HOST_ASAN` (`sly1_headless`) and the devkitA64
   toolchain file (`sly1.nro`).

## 4. The 64-bit strategy

### The choice: keep retail's layouts, make the game's pointers 32-bit

There are two ways to run ILP32 code on LP64.

**(a) Let the structures grow** (pointers become 8 bytes; change `sptr`/`uptr`, fix the 301
size checks, the descriptor sizes, the `count * 4` allocations, the 65 raw offsets, widen
the script value). This is what the brief's checklist describes. I recommend against it,
for one measured reason: 843 structures are local views and 1,109 paddings are written as
absolute offsets. When a pointer member grows, every field after it moves, but a padding
written `0x2E0 - 0x2D4` does not, and another file's view of the same object, which pads
over that pointer instead of declaring it, does not move at all. Two files would then read
one object at different offsets. Nothing detects that: it compiles, ASan sees a valid
object, and the game misbehaves. The checklist covers the known places; this failure is in
the places nobody listed.

**(b) Keep every layout as retail's** by making a pointer *stored in the game's memory* a
32-bit value. **Recommended.** It is the model OpenRAC's port chose (OpenGOAL's offsets),
adapted to C++:

- `Ptr32<T>`: a 4-byte, trivially copyable class holding a 32-bit offset from one anchor
  symbol in the program image; 0 is null. It converts to and from `T*`, and has `->`, `*`,
  `[]`, arithmetic, comparison and call operators, so the game's expressions compile as
  written.
- The generation step rewrites **pointer types in declarations** (`T*` becomes
  `Ptr32<T>`: members, globals, parameters, locals, casts, `sizeof`). Expressions are not
  rewritten. `this` and temporaries stay native.
- Everything a game pointer can point at is inside the image, so within 2 GiB of the
  anchor on every platform: globals and functions already are; the game's heap is a static
  array (the PS2's 32 MiB, configurable); the game's threads run on static stacks. So one
  offset works for data, functions and stack objects alike, with no table of functions and
  no fixed load address (which neither macOS on Apple Silicon nor the Switch allows).

What that buys, compared with (a):

| Item of the brief's checklist | Under (b) |
| --- | --- |
| 301 `check_` size assertions | hold unchanged, and stay in the build as tests |
| class sizes in the descriptors, `count * 4` allocations | correct as written |
| 65 raw-offset accesses | correct as written |
| script values holding handles in 32 bits | correct as written: a handle *is* 32 bits |
| `sptr` / `uptr` | stay `s32` / `u32`; a cast to them yields the 32-bit offset |
| records read as raw blocks (shader, texture table) | unchanged |
| retail comparison | every object has retail's offsets, so its state can be compared field by field with a retail memory dump |

The failure mode also changes: a place the rewrite gets wrong is a **compile error** (a
type mismatch), not silent corruption.

### What (b) costs, and its risks

- A real tool: the rewrite needs the AST, not regular expressions (libclang).
- A long tail of compile errors on first contact (conditional expressions mixing `Ptr32`
  and literals, variadic calls, 13 templates). Each is fixed in the tool or by a named
  patch, never in the submodule.
- Tables initialised with addresses become dynamic initialisers. They get an early
  initialisation priority so the game's own static initialisers see them filled.
- **Layout rules differ between g++ 2.95 and today's ABI** for derived structures (reuse of
  a base's tail padding) even with 4-byte pointers. Defence: the step generates an
  `offsetof` assertion from each of the 5,596 offset comments, next to the 301 size checks;
  a difference stops the build and is fixed with explicit padding in a patch. Windows uses
  Clang with the GNU ABI (MSYS2 `clang64`), not `clang-cl`, whose layout rules differ again.
- One add per pointer load. Negligible for this game.
- A pointer to memory the port allocated outside the image cannot be stored in a game
  structure. The platform layer therefore allocates anything it hands the game from the
  game's arena; debug builds assert the range in `Ptr32`'s constructor.
- ASan does not see inside the static heap. The arena is poisoned and each allocation
  unpoisoned at the game's allocator entry points; stack-use-after-return detection is
  turned off, because it moves locals out of the static stacks.

This is the largest technical risk of the port, so milestone 1 starts with a spike on it
(section 9) and a decision point before the rest is built on it. If the spike fails, the
fallback is (a) with a generated cross-check of every view against its class, which is
more work and weaker.

## 5. What replaces `src/lib/`

Nothing of `src/lib/` or `src/libm/` is compiled, with one possible exception (videos,
section 8). `port/platform/` defines the functions the game calls, under the same symbols,
found by role in `config/names/lib/`:

| Role in the decompilation | Port |
| --- | --- |
| C and C++ runtime, formatted output, sort, random, memory and string functions | the platform's, or small own versions where the game depends on the exact result (the random generator, the formatter's format set) |
| kernel calls: threads, semaphores, interrupts, timers, cycle counter | host threads on static stacks; semaphores; a vertical-blank event from the presenter; a counter derived from real time, or from the frame count in deterministic runs |
| CD sector read (0x203B18) and its status calls | reads of the `.iso` at sector × 2048; on a worker thread on Switch |
| DMA and graphics helpers, the packet submit | the renderer's intake (section 7) |
| pad | libnx pad and vibration; scripted or idle input on the host |
| memory-card client | files in a save folder, behind the same asynchronous calls the save code polls |
| IOP clients and the command channel | the sound engine's command queue; every call answered (section 8) |
| MPEG library, IPU | the video decoder (section 8) |
| exception and unwind runtime, `tlb`, patch installers, IOP reset and module loading | the compiler's own runtime; no-ops that report success |

The helper headers: `vu0.h` becomes plain C++ over a 16-aligned four-float value, each
helper implementing its "Computes:" line, including undefined lanes left as they fall and
the divide and square-root cases the lines state. The two microprogram interfaces
(`ParticleFilter`, `NextLightGroup` and its companions) are reimplemented inside
`FilterParticleVerticesVu0` and `CollectLightsForSphere` by a patch, from the microcode's
behaviour.

## 6. Floats and time

- **Floats.** The game thread runs with rounding toward zero and denormals flushed (FPCR
  on AArch64, MXCSR on x86-64). The generation step routes float division and float-to-
  integer conversion in game code through helpers with the EE's results (no infinity,
  saturating), as OpenRAC's does. The `vu0.h` replacement clamps as its header states.
  PORTING.md warns these rules come from public hardware descriptions and are unproved, so
  they are unit-tested against those descriptions and marked as assumptions until compared
  with retail.
- **Time.** `AdvanceGameClock` measures ticks since the last frame, clamps the step to
  1/60–1/30 s and scales it. So the game is already 60 Hz when frames arrive at 60 Hz; no
  game logic changes. The port supplies the tick source and presents on vertical sync
  (sleeping, not spinning). Headless runs feed exactly 1/60 s per frame, so they are
  repeatable.

## 7. Rendering

### First-pass survey of the draw paths

From the names tables and the sources' comments, not yet traced instruction by
instruction; the full survey (`docs/RENDERER.md`) is the first task of milestone 4.

| Path | Where | How it reaches the hardware |
| --- | --- | --- |
| frame structure | `render_frame.cpp`, `SubmitDeferredRequests`, the 18 group-marker rows and their five `Prepare*Group` handlers | up to 1,024 deferred requests a frame are sorted and bucketed by group; each group uploads projection, camera and group constants and selects one of two VU1 program blocks; a render thread sends the finished frame |
| static and placed models | `placed_object_model.cpp`: `BuildStripMeshPacket`, `BuildLitStripMeshPacket`, `BuildPartVifPackets`, `DrawModelPart`, `QueueModelPartDraws`, `QueuePlacedObjectModelParts` | VIF packets built once at load, referenced each frame after per-part culling and fades |
| skinning and morphs | the same builders: skin-weight, position-delta, normal-delta and morph-target programs, `BuildMorphTargetPacket` | extra unpacks and program calls per mesh |
| lit and unlit batches | `BuildLitVertexPacket`, `BuildUnlitVertexPacket`, `FlushVertexBatch` | dynamic vertex batches |
| particles | `WriteListedObjectParticlePacket` (three programs), `FilterParticleVerticesVu0` | VU1 programs; a VU0 filter |
| effects | `effect.cpp` (15 draw functions), `effect_spawner.cpp` | dynamic batches |
| trails and lines | `trail_ribbon.cpp`, line meshes, `DrawLineSegment` | dynamic batches; GS lines |
| links and cones | `range_record.cpp`, `RefreshConeVolumeShape` | own programs |
| coloured triangles | `colored_triangle_set.cpp` | dynamic batches |
| 2D elements and fonts | `screen_element.cpp` (37 elements), `font.cpp`, `arc_element.cpp`, `text_panel.cpp` | sprites and quads |
| fades, display | `display.cpp` (`FadeScreenToBlack`), `color_fade` | full-screen GS packets |
| textures, render targets | `texture_upload.cpp` (`StartImageUpload`, `StartPaletteUpload`, `SetupTextureRenderTarget`) | GS memory transfers by load class; a texture as draw target |
| videos | `movie_player.cpp` (`UploadMovieFrame`, `DrawMovieFrame`) | decoded picture uploaded and drawn as a sprite |

### Recommendation: read the game's own packets, one native handler per program

The game keeps doing everything it does today: culling, fades, sorting, group order,
packet building. The port takes over where the packets would go to the hardware:

- a reader walks the frame's chain and its VIF commands (public formats). It does not run
  VU1 code: each program entry point (the named ones, by role) has a **native handler**
  that knows that program's inputs and turns them into a draw with the matching shader;
- GS register writes (state, 2D primitives, uploads) are translated to pipeline state,
  draws and texture updates. Textures are identified through the game's own texture
  tables, not by modelling GS memory;
- a model packet built at load is converted **once** into a GPU vertex buffer, keyed by
  the packet's address and dropped at level unload: that gives static geometry in GPU
  memory without touching the builders. Dynamic batches go to a ring buffer.

Why not intercept at the builders instead: it would mean patching dozens of game
functions and re-deriving the frame's ordering and state, each a chance to differ from
retail; the packet boundary is narrow, is where the game's decisions are already final,
and lets every draw path be brought up and tested one handler at a time, with a counter
of packets no handler claimed. How lighting, skinning, outlines and fades work is **not
assumed here**; it is established per handler from the builders and the microprograms'
behaviour, and written down before each is implemented.

The front end (reader, handlers, draw list) is independent of the graphics API. Back
ends: **deko3d** on Switch (GLSL 450 through `uam`, 1280×720 and 1920×1080), and a
**host reference rasteriser** in plain C++ that draws the same list to a PNG. The second
exists because I cannot see a Switch screen: it makes `--dump-fb` work on the host and in
CI. It does not prove the deko3d shaders; those need you or an emulator.

Widescreen and HD textures hang off this design as the brief describes them (the
projection and culls in game code through patches; an override lookup where a texture is
identified).

## 8. Sound and video

**Sound.** `port/audio/` impersonates the driver at the command queue of
`iop_commands.cpp`: every command gets its reply, including the ones the game busy-waits
on, and an unknown command is logged and answered rather than left pending. The engine:
bank parsing from DISC.md's layout, SPU ADPCM decoding, voices with the ADSR words, volume
(0x400 is 1.0) and pitch words, a sequencer for the level banks' MIDI-form streams, and
VAG streams for music and voice, mixed at 48 kHz into libnx audio on Switch and into WAV
files on the host. Bank fields only the driver used are unknown; where a field's meaning
would have to be guessed, it is listed in `docs/AUDIO.md` as unmeasured, and compared with
recordings of real playback that you supply. The driver is never disassembled.

**Video.** The 17 videos are replaced at the calls `movie_player.cpp` makes. Two ways to
decode, and the choice has a dependency in it, so it is yours:

- (i) keep the decompilation's own MPEG demultiplexer and decoder control (compiled like
  game code) and write only what the IPU hardware did (variable-length decoding, inverse
  transform, colour conversion) from the public standard. No dependency; closest to retail;
  more code of ours;
- (ii) decode with FFmpeg's `libavcodec` (LGPL/GPL, AGPL-compatible, already in devkitPro's
  portlibs). Less code; a large dependency on four build environments.

I recommend (i), decided for good after measuring one stream's format in milestone 3.

## 9. Test strategy

On the host, with ASan and UBSan, in this order of trust:

1. **Layout**: the 301 size checks and the generated offset assertions, at compile time.
2. **Unit tests without a disc** (run in CI on macOS, Windows and Linux): `Ptr32`, each
   `vu0.h` helper against its "Computes:" line, the float helpers, LZSS, SPU ADPCM, the
   packet reader on synthetic packets, the proprietary-material check.
3. **`--verify-levels`** (needs the disc): all 46 levels through the game's own loaders,
   60 ticks of the object loop each, no sanitizer report, and the load ending where DISC.md
   says the objects must end. Run twice: identical state hashes prove determinism.
4. **Frames and samples**: `--dump-fb` through the reference rasteriser, WAV dumps.
5. **Against retail**, where you can supply it: memory dumps or recordings from your own
   console or emulator, compared field by field (possible because layouts are retail's).
6. **The decompilation itself**: `tools/build.py` in the submodule still prints `MATCH`,
   and `git -C sly1-decomp status` stays clean, checked at every milestone.

A repository check modelled on the decompilation's `tools/proprietary.py` and
`tests/check_repository.py` (SPDX lines, no game data, no binaries) runs in CI from the
first code commit.

## 10. Milestone 1, in steps

1. Repository scaffolding: CMake with both targets, the proprietary check, CI.
2. **Spike** (decision point): `Ptr32` and the generation step on six representative files
   (`base_object.cpp`, `buffered_stream.cpp`, `heap.cpp`, `script_machine.cpp`,
   `placed_object_model.cpp`, `matrix.cpp`), compiled for the host with the layout
   assertions. I report the error counts and what the rewrite needed, and we confirm (b)
   or fall back before going on.
3. All of `src/main` generated and compiling; the helper headers; the 48 assembly
   functions; platform stubs.
4. `.iso` sector reads; boot to the level loader; `--verify-levels`.

## 11. Decisions for you

1. The pointer model: (b), 32-bit game pointers with retail's layouts (recommended), or
   (a), growing structures as the brief's checklist assumes.
2. A host reference rasteriser as a second back end (recommended), or deko3d only,
   verified by you on the Switch Lite or in an emulator you install.
3. Video decoding: (i) own code behind the IPU's role (recommended), or (ii) FFmpeg.
4. Whether you can supply retail references (memory dumps, audio recordings) for the
   comparisons in sections 8 and 9.
5. `libclang` (Apache-2.0 with the LLVM exception) as a build-time tool dependency.

## Credits

The pointer model and the float handling follow OpenRAC's port design (its
`docs/port/DESIGN.md` and `hostgen`), read for this note; no code is taken yet. Anything
borrowed later is credited in the commit and in `THIRD_PARTY_NOTICES.md`.
