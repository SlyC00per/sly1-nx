> **Added 2026-11-14 by the maintainer:** the port matches the PlayStation 3 release as
> closely as possible: widescreen, HD textures (part of the target, not optional), no memory
> cards, and the other differences in `docs/PS3_PARITY.md`. Where this brief says a
> difference from retail is a bug, the differences listed there are the exceptions.

I want a native Nintendo Switch homebrew port (AArch64, libnx, deko3d) of *Sly Cooper and the Thievius Raccoonus* (PS2, NTSC-U `SCUS_971.98`), built from our finished matching decompilation. Goals: the whole game playable at a locked 60 FPS, true 16:9 widescreen, low power use in handheld mode, and optionally the PS3 collection's HD textures in place of the PS2 ones.

You are starting with no context from the sessions that produced the decompilation, so this brief tells you what exists, what is known, and where the hard parts are. Read it fully, then the documents listed under "Read first", before writing code.

## The repositories

- **This repository** (the port). It is new: it holds the port's code, build and docs, and takes the decompilation as a git submodule at `sly1-decomp/`, pinned to a commit. Updating the decompilation is a deliberate submodule bump, never an edit.
- **`OpenThievius/sly1-decomp`** (the decompilation, finished; the submodule). Every function of the game is C++ that rebuilds the retail executable byte for byte with the original 1999 compiler. It is the regression test for everything, and it has strict rules of its own (`AGENTS.md`). **The port does not change it.** Its maintainers' ruling: portable code belongs in a port, not behind `#ifdef`s in the decompilation. If you find a real bug or a byte-neutral cleanup the decomp should have, stop and tell me; do not edit it yourself.
- **`OpenThievius/OpenThievius`** (the project's front page; optional reading). Its `docs/ROADMAP.md` lists two open port decisions, and `docs/policy/SOURCING.md` the sourcing rules this port follows too.

No game data, disc image, executable, compiler or SDK file is ever committed. Add a check to this repository that enforces it (sly1-decomp's `tools/proprietary.py` and `tests/check_repository.py` are a model). Assets come from my own disc image at run time.

## Read first

In `sly1-decomp/`: `AGENTS.md` (rules and the map of the repository), `docs/PORTING.md` (where every PS2-specific part is, written for you), `docs/DISC.md` (how the game finds and reads its data, every level format, the sound banks), `docs/OBJECTS.md` (what each object class and animation event is), `docs/TOOLCHAIN.md` (compiler behaviour the code depends on), `docs/NAMING.md` and `config/unnamed.tsv` (what the names mean and what is still unnamed). In OpenThievius (optional): `docs/ROADMAP.md`, `docs/policy/SOURCING.md`.

Names in the decompilation were derived from the code, not from the original game: do not expect the original developers' names, and do not introduce names remembered from the game or from other projects.

## What is true of this game (correcting common assumptions)

These matter because several things that are true of other PS2 games, and of the Sly sequels, are not true here.

- **There are no `.WAD` files and no file names.** The disc's file system lists only the executable and IOP modules. Everything else is in unlisted sectors, found through 32-byte records in the executable's data (start sector and size, XOR-hidden; tables for 46 levels, 17 videos, 7 sound banks, music and voice streams). The simplest faithful port keeps the game's own loaders and implements the low-level sector read against my `.iso` on the SD card.
- **Levels are streams, not memory images.** A level is one LZSS-compressed file that the game's loaders read front to back, field by field, through a buffered stream (`src/main/buffered_stream.cpp`), building their run-time structures as they go. So there are few serialized pointers to fix up; a `WadPtr<T>` arena is not the model. The 64-bit work is in the run-time structures instead (below). A few records are read as raw blocks into structs (for example the 20-byte shader record and 6-byte texture table); those structs must keep their exact packed layout.
- **There are no Sony SDK headers or names in the source.** The libraries the game linked (kernel calls, DMA and graphics helpers, pad, CD, memory card, IOP clients, the MPEG/IPU decoder, the C and C++ runtime) are reconstructed in `src/lib/` and `src/libm/` under the project's own names, each commented with what it sends to the hardware or the IOP and what the game waits for. The port replaces this layer; search by role, not by SDK name.
- **The game already runs at 60 Hz on NTSC,** with animation times in 1/60 s ticks. Find how the game clock derives its time step (`src/main/game_clock.cpp`, the main loop in `src/main/main_loop.cpp`) before assuming anything needs "locking".
- **Sound is not SPU register writes.** The game sends commands to Sony's sound driver on the IOP: load a bank by sector, start a sound by bank handle and record number with volume and pitch words, adjust and stop the returned voice (`src/main/sound_system.cpp`, `src/main/iop_commands.cpp`; `docs/DISC.md`, "Sound banks"). The driver itself is not decompiled and must not be disassembled. The port answers those calls with its own engine: sound effects from the bank records' tones (SPU ADPCM samples with ADSR words), level music from the level banks' MIDI-form sequences, and VAG streams for music and voice. Bank fields only the driver used are unknown and need measuring against real playback. The game busy-waits on some driver replies, so every call must be answered.
- **Videos are MPEG program streams** decoded by the reconstructed MPEG library through the PS2's IPU hardware. The port needs a software decoder for the 17 videos (video and audio), behind the same calls the game makes.
- **Graphics goes through packets the game builds itself,** and the VU1 microprograms that consume them are original assembly (`.vutext`, about 0x5550 bytes) with no C. The model loader reads plain arrays (positions, normals, colours, UVs, vertex records, bone weights, morph targets) and builds VIF packets from them (`ReadPlacedObjectModel`, `BuildStripMeshPacket`, `BuildLitStripMeshPacket`, `DrawModelPart`, in `src/main/placed_object_model.cpp` and `game_progress.cpp`); 18 microprogram entry points are named by role. Do not assume how lighting, skinning, outlines or fades work: establish it from those builders, the named entry points, and the microprograms' behaviour. Bone weights are per position and bone, not a fixed four.
- **Vector math goes through one seam,** `include/vu0.h`, whose helpers each carry a "Computes:" line saying exactly what they produce per lane. A port replaces that header (and `fpu.h`, `system.h`, `mul_wide.h`) with portable bodies of the same names. Note the float conventions it documents (no NaN or infinity, round toward zero, saturating divides): OpenRAC's port of a sister engine found gameplay drifts from retail without them in collision, steering and camera code.

## The hard parts, so you can plan around them

1. **64-bit pointers.** The PS2 is ILP32; Switch and the host are LP64. In the decompilation:
   - integers that hold addresses are already `sptr`/`uptr` (`include/common.h`), two typedefs to change;
   - every structure has a `check_` size assertion that assumes 4-byte pointers, and class sizes are also stored in the class descriptors the game allocates objects from, with some array allocations sized as `count * 4`;
   - 65 raw-offset accesses remain, each with a comment saying why (search for those comments: they are your checklist);
   - script values hold object and sound handles in a 32-bit word (needs a wider word or a handle table);
   - every declaration agrees with its definition, so the source links with a normal linker.
2. **Code a modern compiler rejects.** `python3 tools/modern_check.py` in sly1-decomp reports it. Game code is at zero errors, but three things remain by design and the port must handle them without editing the decomp: nine functions using g++ 2.95 named return values, the `<?`, `>?` and `>?=` operators, and the old compiler's own varargs and exception-runtime internals. The check already rewrites the first two on a copy of the tree; a build step of the port can do the same. Code shaped oddly to match retail (`goto`s, statement order) must not be "cleaned up".
3. **Rendering without the VU1 programs** (above). Decide early between intercepting at the model and packet builders, and interpreting the packets; survey every draw path first (models, skinned and morphed meshes, particles and effects, trails and lines, the 2D screen elements and fonts, fades and transitions, render-to-texture targets).
4. **Sound and video** (above): both need engines written from the documented formats and the game's calls.

## How I want you to work

1. **Start with a structural summary and a design note, not code.** Inspect this repository and the submodule, then write `docs/DESIGN.md` covering: how the port builds the decompiled sources unmodified (include-path overrides for the replaced headers, the source rewrite step, what replaces `src/lib/`), the 64-bit strategy for structure sizes and script handles, the rendering approach with the draw-path survey, the audio and video plan, and the test strategy. `docs/ROADMAP.md` lists two open decisions (building the sources directly or through a generation step as OpenRAC does; language and build system): recommend one each with reasons. Show me the note and wait for my go-ahead.
2. **Then work in milestones,** each ending with a build, its verification, a commit, and a short summary of what you found. Do not start the next until the current one verifies.
3. **Verify on the host first.** A headless host build with AddressSanitizer and UBSan is the main test loop, because you can run it yourself. Tell me at the start what you can and cannot run in your environment (devkitA64, an emulator, hardware over nxlink); anything you could not run, say so plainly in the summary instead of reporting it as working.
4. **Compare against retail where you can.** The decompilation is byte-exact, so a behaviour difference is a port bug. Prefer tests that load every one of the 46 levels and run the object loop, over spot checks of one level.
5. **Keep the matching build intact.** In `sly1-decomp/`, `python3 tools/build.py` must still print `MATCH: build/us/SCUS_971.98 sha1 57dc305d…` (it needs my disc and the period compiler; see its README). You should have no reason to change that repository at all.
6. **Ask before decisions that are mine:** anything about distributing the port or its assets, adding a dependency with a licence other than AGPL-compatible, or changing a goal below.

## Milestones

**1. Host build, 64-bit clean.** A CMake project at this repository's root with a `HOST_ASAN` target (and the devkitA64 target configured, even if it only builds later). Compile all of sly1-decomp's game code for LP64 with portable replacements for the helper headers and stubs for the library layer; fix structure sizes and handle storage by the strategy in the design note; implement sector reads from an `.iso`. *Verify:* `sly1_headless --iso <path> --verify-levels` loads each of the 46 levels through the game's own loaders and runs 60 ticks of the object update loop with no ASan or UBSan report; a level's load consumes exactly the bytes `docs/DISC.md` says it should.

**2. Switch bring-up: files, input, saves, pacing.** Entry point and nxlink logging; the `.iso` read on a worker thread; pad input and vibration mapped from the game's pad layer (`src/main/pad_input.cpp`, `src/lib/pad.c`); memory-card saves through the game's own save code (`src/main/memory_card.cpp`) onto the SD card or save data; frame pacing that sleeps on vsync instead of spinning. CLI flags for unattended runs (`--level`, `--frames`, `--dump-fb`).

**3. Sound and video.** The sound engine behind the driver's calls (effects, level-music sequences, VAG streams), and the software video decoder behind the MPEG library's calls. *Verify:* every command the game sends is answered (no hang on any level's load or start); decoded samples and frames dumped on the host for me to check by ear and eye.

**4. deko3d renderer.** By the approach agreed in the design note: geometry, skinning and morphs, the PS2 colour convention (0x80 is 1.0), the GS depth, alpha-test and blend states the game sets, palettised textures (4- and 8-bit indices with CLUTs in the GS's CSM1 order, alpha up to 0x80; type-0 shaders draw one image three times with three CLUTs), 2D elements and fonts, fades and transitions. 1280×720 handheld and 1920×1080 docked. Static geometry in GPU memory, a ring buffer for dynamic vertices. Shaders in GLSL 450 compiled with `uam`.

**5. 16:9 widescreen.** Hor+ (vertical field of view preserved): the projection (`UpdateCameraProjection` and the camera's `aspectScale` in `src/main/camera.cpp`), the frustum and visibility culling (`BuildFrustumSideNormals`, the per-part culls in the draw path) so nothing pops at the sides, the 2D screen elements (`src/main/screen_element.cpp`) anchored without stretching, and videos pillarboxed. Check places that hard-code 4:3 (search for `1.33333337f`).

**6. Optional HD textures.** A hash over each PS2 texture's index data, CLUT, size and format selects an override from `/switch/sly1/hd_textures/`, loaded as BC1/BC3; the shader samples it directly instead of the CLUT path; texture coordinates and clamp regions stay relative to the original PS2 dimensions; a runtime toggle. Plus an offline tool that pairs my PS2 and PS3 textures. The PS3 release's file formats are not documented anywhere in these repositories: research them from my own copy, and tell me early if pairing by structure order and perceptual hash does not work. HD assets are mine to supply and are never committed.

## Constraints

- Graphics on Switch through deko3d directly (no OpenGL/Mesa layer), for CPU overhead and battery.
- **The port builds on macOS, Windows and Linux.** CMake and Ninja for both targets; the Switch target through devkitPro (`$DEVKITPRO`, never a hard-coded `/opt/devkitpro`; on Windows devkitPro installs under MSYS2); the host target with Clang or GCC (ASan and UBSan; on Windows, Clang through MSYS2 or Visual Studio's clang-cl). Helper scripts in Python or CMake, not shell-only. Paths in code and tools are relative or configurable, and nothing assumes a case-insensitive file system or `/` paths. A CI matrix (GitHub Actions: macOS, Windows, Linux) builds the host target and, with devkitPro's container, the Switch target, and runs the host tests that need no disc.
- AGPL-3.0-or-later for everything, with the SPDX line the repositories' checks require; commits follow sly1-decomp's `CONTRIBUTING.md` conventions (Conventional Commits, a body saying what was verified, no AI attribution trailers); add a `LICENSE` (AGPL-3.0) and a `README.md` saying the game's data comes from the user's own disc.
- Nothing from Sony's SDK, leaked material, the game's prototype, or other Sly decompilations or ports. OpenRAC (the sister project, same maintainers) is the one source you may borrow port code and ideas from, credited as sly1-decomp's `LEGAL.md` and OpenThievius's `docs/policy/SOURCING.md` describe.

Begin by checking that the submodule is present and on sly1-decomp's latest master (initialise it if not), then the structural summary and the list of what you can run in your environment, then the design note.
