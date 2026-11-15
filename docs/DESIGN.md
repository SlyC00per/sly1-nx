# Design note

Status: **accepted** (maintainer's go-ahead, 2026-11-14), with the decisions of section 11.
Where a claim was measured, the command's result is given; where it is a plan or an
assumption, it says so. Written on 2026-11-14 against `sly1-decomp` at `8f7a85a3`.

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

- `Ptr32<T>`: a 4-byte game address (an offset from the program image's start; 0 is null).
  It converts to `T*`, is assigned from `T*`, and has `->`, `*`, `[]`, arithmetic,
  comparison and call operators, so the game's expressions compile as written. It is plain
  old data with **no constructors** (the spike showed why, below): a stored pointer's
  initialiser is wrapped in `port::P32(...)` by the generator.
- The generation step rewrites **pointer types in declarations** (`T*` becomes
  `Ptr32<T>`: members, globals, parameters, locals, casts, `sizeof`). Expressions are not
  rewritten. `this` and temporaries stay native.
- Main memory is mirrored: a static 32 MiB block, `port_ram`, holds what the PS2's main memory
  held at the same addresses. Start-up copies the executable's segments from the player's disc
  into it, then each file's initial values (the generator renames every C definition of a
  `D_XXXXXXXX` global to `port_init_D_XXXXXXXX` and writes the copy). Every global is a macro
  over its retail address, so neighbouring globals that the C splits from one object (a
  decompilation leftover) still sit together as in retail, and the heap ends at 0x02000000 as
  retail's does. Addresses 0x0 to 0x02000000 are `port_ram`; 0x08000000 and up are the
  port's program image (functions, string literals, statics); the scratchpad, the hardware
  registers and the uncached alias have their PS2 addresses. No fixed load address is needed
  (neither macOS on Apple Silicon nor the Switch allows one).
- Functions are stored as their **retail addresses**, as the executable's data holds them
  (class descriptors, callback tables): the generator writes a table of every `func_XXXXXXXX`
  the C defines (and the port's own definitions of the assembly functions), and a call through
  a stored pointer looks the native function up there.
- Declarations whose result disagrees with the definition's, pointer against integer (they link
  because a C++ link name leaves out the return type), get a per-file adapter that converts
  between game address and pointer. The list goes to `build-host/gen/result_clashes.txt`.

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
| memory-card client | save data, behind the same asynchronous calls the save code polls; as on the PS3 release, no memory card: the card checks, slot selection and card prompts are bypassed by patches (`docs/PS3_PARITY.md`) |
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

### The anti-tamper checks

Retail has two checks for a modified executable (a patched copy, a cheat device, a loader's
hooks), found while giving the game's symbols a place in memory:

- **The code checksum** (`VerifyCodeChecksum`, called once when the render thread starts): a
  CRC-32 over `0x148D74` bytes from `0x100000`, about 1.3 MB, all of the executable's code
  including its controller and kernel libraries. A nonzero result sets `g_codeChecksumFailed`.
  Then, after a random 30 to 120 seconds of play, the function that picks the player's next
  action from nearby targets keeps its current state instead (`player_target.cpp`): Sly stops
  latching onto ledges, hooks and other context moves.
- **The memory checksum** (`StepMemoryChecksum`, 32 words per world update): a running CRC from
  `0x100004`. A nonzero result at the end of a pass sets `D_00275C34`. Then each update has a 20%
  chance of marking one of the level's collected items uncollected again (`object_gatherer.cpp`),
  and a level-record counter stops counting (`state_notifier.cpp`).

Both are quiet and delayed, so that the effect looks like a bug and is hard to trace back to the
check, a common design around 2000. A known real-world effect: Open PS2 Loader's In-Game Reset
hooks the game's code, so without its compatibility Mode 6 (which disables the reset) the hook
stops working early in the first level. This explanation is ours, from the code; the developers
have not described the checks.

The port's code is not retail's machine code, so both checks could never pass. Two named
patches (`tools/patches.py`) remove only the lines that set the flags: the checks still run and
the game keeps the outcome a genuine copy has, as the port's rule of retail behaviour requires.

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

Widescreen and HD textures are part of the target, since the port matches the PS3 release
(`docs/PS3_PARITY.md`); they hang off this design as the brief describes them (the
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

**Decided (maintainer, 2026-11-14): (i) for the PS2 videos.** If the PS3 release's videos are
better than the PS2's and are wanted (`docs/PS3_PARITY.md`), they are decoded with FFmpeg as
optional replacements found on the SD card, as the HD textures are; FFmpeg is added only then.

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
   **Result (2026-11-14): (b) holds on the spike.** `tools/gen.py` and
   `tools/gen_pointers.py` rewrote 1,191 declarations, 44 cast types, 34 pointer-to-integer
   and 34 integer-to-pointer casts and 3 conditional arms in the 65 files the six units
   reach. Before the pointer stage clang++ reported 381 errors in the six units (347 failed
   size checks, 34 pointer casts); after it, all six compile to object code with 0 errors,
   so every size check they reach holds, and `tools/layout_check.py` compares 2,640 retail
   field offsets with the host compiler's layout and finds 0 differences. What the rewrite
   needed along the way: libclang given the system compiler's include directories (it has
   none of its own); `-fms-extensions` for the analysis only, so the casts a 64-bit compiler
   rejects stay in the tree; no `nullptr_t` constructor in `Ptr32` (`p = 0` was ambiguous)
   and an explicit one from other pointer types (casts to typedefs of stored function
   pointers); null constants left out of the conditional-arm rule.

   **Over all 101 units (2026-11-15): (b) holds; two design changes.** With the Clang host
   compiler 97 units compile and with the Switch's GCC 100 (strict, no `-fpermissive`); every
   remaining error is the old compiler's variable-argument builtins (`__builtin_next_arg`,
   eight functions in four files), which `modern_check.py` already lists as a port's job and
   which get named patches. 4,860 retail field offsets are asserted in the generated code and
   hold on both compilers. The rewrite: 3,076 declarations, 551 cast types, 1,152
   integer-to-pointer and 924 pointer-to-integer casts, 10,926 stored-pointer initialisers,
   156 conditional arms, 301 variadic arguments, 6 addressed parameters; about 80 seconds.
   What the full run found:
   - **Tail padding.** Today's ABI (Clang and GCC) puts a derived class's first members in its
     base's tail padding when the base is not C++03 POD, which a class with a base of its own
     never is; g++ 2.95 never does. `tools/gen_layout.py` measures each base's data size with
     the compiler itself (a probe class derived from it) and gives the one class that needs it
     an explicit tail member (`LightObject`'s base, 4 bytes).
   - **`Ptr32` without constructors.** GCC also reuses padding next to members with
     constructors, and refuses them in anonymous unions and structures (1,384 errors): a
     `Ptr32` is now plain old data, and the generator writes `port::P32(...)` where a stored
     pointer is initialised (declarations, braced lists by their target field, constructors'
     member initialisers, addressed parameters). Typedefs of pointers stay native.
   - **Offset assertions.** Every commented retail offset becomes a
     `static_assert(offsetof(...))` after its record, so each compiler checks the layout itself.
     Records that are views of a larger object (their comments give the outer offsets) are set
     aside; records that differ are reported, not asserted.
   - **Virtual functions.** The movie player's `TransferSink` family (three classes, the only
     ones with virtual functions outside the C++ runtime) has an 8-byte hidden table pointer on
     64-bit: `MovieStream` and `MoviePlayer` differ from retail. A named patch with an explicit
     4-byte table pointer, with the video work (milestone 3).
   - **Named patches** (`tools/patches.py`): exact replacements in the generated copy, each with
     its reason, failing the generation if one stops matching. One so far (`PROP` in
     `property_registry.cpp`, whose pointer is in the macro's body). Left for patches: the eight
     variable-argument functions, `AT` in `player.cpp` (a raw-offset macro that may receive an
     integer address), one integer-to-pointer cast inside a macro argument
     (`placed_object.cpp:4878`) and one field declaring its own type (`collider.cpp:1836`).
   Not yet checked: anything at run time.
3. All of `src/main` generated and compiling; the helper headers; the 48 assembly
   functions; platform stubs.
4. `.iso` sector reads; boot to the level loader; `--verify-levels`.

## 11. Decisions (maintainer, 2026-11-14)

1. **The pointer model: (b)**, 32-bit game pointers with retail's layouts, confirmed or
   reversed by the spike of section 10.
2. **A host reference rasteriser** as a second back end, beside deko3d.
3. **Video decoding: (i)** for the PS2 videos; FFmpeg only for PS3 replacements, if wanted
   (section 8).
4. **Retail references** (memory dumps, recordings) when the maintainer can supply them.
5. **`libclang`** (Apache-2.0 with the LLVM exception) as a build-time tool dependency.

## Credits

The pointer model and the float handling follow OpenRAC's port design (its
`docs/port/DESIGN.md` and `hostgen`), read for this note; no code is taken yet. Anything
borrowed later is credited in the commit and in `THIRD_PARTY_NOTICES.md`.
