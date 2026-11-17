# The renderer (milestone 4)

How the port draws the game, and the work left. The decisions are in docs/DESIGN.md, section 7.

## What reaches the GS

From `SLY1_RENDER_SURVEY` over the opening and The Hideout (2026-11-16):

- **3D goes through VU1.** VIF1 unpacks vertex data (V4-32, V4-8, V3-32, V4-16 most) into VU1's
  data memory and starts microprograms; what they send with `XGKICK` (path 1) is triangle
  strips and fans, some lines, textured with 4- and 8-bit palettes.
- **2D, fades and texture uploads go through the GIF channel** (path 3): sprites, the IMAGE
  transfers (formats 0x00, 0x14, 0x13), register setup. VIF1's DIRECT (path 2) carries a few
  hundred register packets.

The microcode is two program sets, uploaded in chunks of up to 256 instructions
(`tools/vu1_disasm.py` lists them from the player's disc, into a build folder):

| Set | Chunks (address, hash) | Size |
| --- | --- | --- |
| A | 000 E1624411, 100 6744EAB9, 200 B90790F3, 300 79A6F5FB, 400 2DDC9AA2, 500 8BF630F1, 600 C0012AA9, 700 51A89CD8 | about 1,880 instructions |
| B (loaded over A's start for some groups) | 000 19FE9C91, 100 1AE4A8D6, 200 7B92AF0F | about 730 |
| a 4-instruction stub | 000 B81FD1EC | 4 |

## The entry points, by calls

Over the opening and The Hideout (about 120 frames and two loads):

| Calls | Entry | Chunk | Notes |
| ---: | --- | --- | --- |
| 25,353 | 0x356 | 79A6F5FB | transforms a vertex buffer in place (4x4 matrix, CLIP, divide by w), backface test by the sign flag, calls the clipper at 0x274 for what crosses the guard band; no XGKICK |
| 25,353 | 0x002 | E1624411 | called with 0x356 every time |
| 21,707 | 0x01E | E1624411 | |
| 9,889 | 0x1A6 | 6744EAB9 | |
| 5,741 | 0x318 | 79A6F5FB | |
| 3,647 | 0x17E | 6744EAB9 | |
| 3,647 | 0x051 | E1624411 | |
| 3,646 | 0x242 | B90790F3 | |
| 3,447 | 0x07B, 0x077 | E1624411 | |
| 3,174 | 0x31F | 79A6F5FB | |
| 1,028 | 0x34A, 0x05F | 79A6F5FB, E1624411 | |
| under 1,000 | 0x001, 0x07E, 0x26D, 0x1F4, 0x1DB, 0x253, 0x350, 0x2DA, 0x110, 0x354, 0x011, 0x0F8 | | |
| rare | 0x000 of set B, the stub | | |

The programs work together through VU1's memory and registers (one entry's results are the
next one's input), and they are scheduled by hand around the VU's latencies (software-pipelined
loops, results read just before or after Q and the flags change).

## The pieces

| Piece | Where | State |
| --- | --- | --- |
| VIF1 (UNPACK with masks, modes and double buffer; MPG; program starts) | port/platform/dmac.cpp | done |
| VU1 interpreter, and the microcode translated to C++ at build time | port/render/vu1.cpp, vu1_runner.h, tools/vu1_translate.py | done: the reference |
| GS model: registers, local memory in the GS's arrangements, uploads, CLUT, primitives | port/render/gs.cpp, gs_memory.h | done |
| Host reference rasteriser and `--dump-fb` | port/render/gs.cpp | done; frames look right (the opening's Paris) |
| The graphics library (double buffer, draw environments) | port/platform/lib_devices.cpp | done, from the decompilation's graph.c |
| What the programs carry between them (SLY1_VU1_LIVE) | port/render/vu1_live.cpp | done |
| Native handlers for the entry points | port/render/vu1_handlers.cpp | 0x356 done (checked over all 46 levels); the rest next |
| deko3d back end (GS primitives on the GPU, texture cache from local memory) | | after the first handlers |

## Speed

VU1 runs about 1.4 million instructions a frame. The translated code runs 150 million a second
on an Apple M2 and 11.3 million on the Switch (about 4 frames a second there): hence the native
handlers, as GPU shaders where the work is per vertex.

## What carries from one program to the next

`SLY1_VU1_LIVE` decodes each instruction's operands while the interpreter runs and records what
each entry reads before it writes it, and which entry last wrote it. Over all 46 levels
(2026-11-16) the entries read little of what came before: vf01 (set by the 4-instruction stub),
and the pipelines' leftovers. The microcode reads registers its loops leave behind: 0x356's
backface test can read the edges an earlier program left in vf14, vf24 and vf29 (below), and
0x411 and 0x47E rotate vf25-vf27 in with MR32. So a handler leaves, besides data memory and the
packets, the registers the report names for its entry (`leaves` in its table row), including
what its loop leaves in them when it reads ahead past the batch's end; the check compares those.

Two rules of the VU the reference needed, found this way:

- **A stall holds the whole instruction.** A lower WAITQ or WAITP (or a DIV or EFU operation
  while the unit is busy) stalls the instruction it is in, so the upper operation beside it reads
  Q or P after the wait (0x37D: `MULq` beside `WAITQ`). The interpreter read them before.
- **A branch in a branch's delay slot.** At 0x27A/0x27B (the guard-band test 0x356 calls), when
  a vertex starts a strip and the triangle it closes is wholly beyond the far plane, both branches
  are taken: the first target runs once, then the second's. 0x28B runs twice and the return lands
  one instruction late, skipping that block's edge and making its backface add unconditional. The
  interpreter does this; the handler reproduces it.

## Where the time goes

`SLY1_VU1_PROFILE` counts the interpreted instructions per entry and times the handlers. Over
levels 1, 10 and 20 without handlers (381 million instructions): 0x356 33%, 0x01E 21%, 0x1A6 10%,
0x002 9%, 0x242 5%, 0x401 3%, 0x26D 3%, 0x076, 0x17E, 0x34A 2-3% each. The 0x356 handler, with the
VU's arithmetic on four fields at once, takes about 2 microseconds a batch on an M2. VIF1's
general UNPACK (byte by byte) shows next to it in a profile: fast paths for the common formats are
due.

## How a handler is written and checked

1. Read the entry in the listing (`tools/vu1_disasm.py`), with the builder that packs its input
   (the decompilation's `placed_object_model.cpp` and the others, docs/DESIGN.md section 7):
   write down its inputs (where in data memory, relative to TOP), its outputs (memory, registers
   a later entry reads, packets kicked) and what it computes, including the order of its float
   operations and the VU's rules (port/render/vu1.cpp).
2. Write it in `port/render/vu1_handlers.cpp`, registered by chunk hash and entry.
3. Check it on the host with `SLY1_VU1_CHECK`: each call runs the handler and the interpreter
   from the same state and compares what the entry leaves (its `leaves`, from `SLY1_VU1_LIVE`),
   the end pc, data memory and the packets kicked, reporting the first difference. The interpreter's result is kept, so a
   wrong handler cannot change the run it is checked in. A handler is done when every level
   runs through its check without a difference.
4. Then move its per-vertex work to the GPU where that pays (a vertex shader computing what it
   computes), checked the same way through the packets.

## The Switch's GPU renderer (2026-11-16)

`port/switch/gpu.cpp` draws what the GS is given with deko3d (`port/gs_backend.h`): runs of the
GS's own vertices (`gs_vsh.glsl`), textures decoded from local memory, the tests and the blend as
GPU state, the fields as whole 960 by 720 pictures. On the Switch, 30 s of the opening: about 17
frames a second, the picture the host's reference draws.

`SLY1_GPU_TRANSFORM` (off unless set) leaves a batch's transform to the GPU: the transform's
handler only notes the batch, and each of its kicks hands the vertices as VIF1 unpacked them, the
matrix and the viewport to `batch_vsh.glsl` (`gs::Batch`). The arithmetic is checked on the host
against the program's (`SLY1_GPU_BATCH_CHECK`: 23.6 million vertices in view, none further than
1/16 pixel, 1 of depth, 1 of fog), and the host draws the batches right in software
(`SLY1_GS_SOFT_BATCH` with `--dump-fb`). On the Switch and in Ryujinx it halves VU1's time, but
from the opening's second scene on most of the picture was black (the sky and the buildings
missing), until the fix below. What was ruled out on the way:

- the renderer is handed the right draws in the right order (`SLY1_GPU_LIST`);
- the first scene (the flag) is drawn right, the later ones are not, in the emulator as on the
  Switch, so the emulator can be used to look for it (`tools/run_ryujinx.py`, `SLY1_GPU_CAPTURE`
  and `tools/gpu_capture.py`);
- not the cause, each tried alone: the depth range of the viewport, depths clamped or cut, which
  side is the front, triangles that reach outside the clip volume, Zcull, the bindings at a change
  between the two kinds of draw, the size of the command memory, the registers the transform
  leaves, the outlines' programs reading a batch after it is drawn (that one is a real dependency:
  a drawn batch is transformed after all when another program follows, `Materialise`);
- a frame's number is not the same moment in every run (the game keeps real time):
  `SLY1_GPU_CAPTURE_HEAVY` starts the capture at the first frame of that many draws instead.

Found (2026-11-16, late): not the GPU at all. The sprites program (0x076 of E1624411: the shadows
under characters, drawn with the destination alpha test) reads the screen positions and STQ of the
lit batch just drawn, in the same half of the data memory. The batch's transform is done in memory
before such a program (`Materialise`), but an upload of the batch's colours (one quadword in four,
by CYCLE) between the transform and its kicks was taken for an upload over the whole batch, so the
transform was never done and the shadows were drawn from the untransformed vertices: large quads
over the screen, which the renderer (without the destination alpha test yet) drew everywhere.
`vu1::DataWritten` now looks at the quadwords an upload writes, the transform keeps the header it
read (the sprites' upload replaces it), and every program but those known not to read the batch
before (`BeforeProgram`) gets the transform done first.

How it was found: `SLY1_LOCKSTEP=N` (N vblanks, each once the game waits and the render thread is
idle, the game's clock made of them) makes two host runs see the same frames, so the inputs of a
program can be compared run against run; `SLY1_GPU_REPLAY` records a frame's draws on the Switch
or in Ryujinx and draws them again cut short (`SLY1_GPU_REPLAY_SEARCH` halves to the draw that
darkens the sky); `SLY1_GS_DATE_TRACE` counts the destination-alpha draws with vertices far off.

Diagnosis switches in the renderer: `SLY1_GPU_CAPTURE` (frames printed, with views of the batches
on the five frames after), `SLY1_GPU_CAPTURE_STEP`, `SLY1_GPU_ZMAP` (the depth buffer as bands),
`SLY1_GPU_STOP_AFTER` and `SLY1_GPU_OMIT` (a frame's draws cut short or one left out),
`SLY1_GPU_SKIP` (kinds of draw left out), `SLY1_GPU_LIST`, `SLY1_GPU_NO_ZCULL`, `SLY1_GPU_CULL_FLIP`.

## Measuring the render thread (2026-11-17)

The Switch is render-bound: its game thread waits for the render thread (core 2) most of each
frame. What follows is how the render thread's work is measured without the hardware, and what
the night of 2026-11-16 found.

- **Determinism.** `SLY1_LOCKSTEP=N` gives the game N vblanks, each once its threads all wait and
  the render thread is idle, and makes the game's clock (`CycleCount`) of them: two runs see the
  same frames however fast they run (for comparing runs, never for playing). A run slowed so much
  that the game is not idle within 100 ms is given its vblank all the same, and is then no longer
  deterministic.
- **Exactness.** `SLY1_GS_COUNT=1` (host) gives the GS a renderer that only counts; with
  `SLY1_GS_HASH=1` it also hashes everything drawn. Normal mode, 900 vblanks: 768 frames, 1,529,944
  runs, 64,483,238 vertices, 60,053,966 indices (1,898,403 runs). With `SLY1_GS_COUNT_BATCHES=1`,
  1,200 vblanks: hash `394B8B349B637BFA`. A change that draws the same keeps these. (Since the
  opening's run was fixed on 2026-11-17; before it 1,529,944 runs and `6E52FB08D049299D`.)
- **The Switch's code.** Ryujinx on an Apple Silicon Mac runs the Switch build's code on the host's
  cores (its hypervisor), so lockstep runs there compare builds' work on the code GCC made for the
  Switch. A read of the clock leaves the guest there, so the per-call timers are opt-in
  (`SLY1_GPU_TIMERS`, `SLY1_VU1_TIMES`, `SLY1_VU1_PROFILE`) and a profiled run there measures mostly
  the clock. The report's "render thread took X s for VIF1's data" is the measure.
- **Where the time goes.** A host build with symbols (`-DPLATFORM=HOST`, `-O3 -g`: the default
  platform is `HOST_ASAN`) sampled with `sample` and the leaves resolved with `atos -i`.

Results, Ryujinx, lockstep 1,200 vblanks, the build before the night against after (both with the
clock fixes): with `SLY1_GPU_TRANSFORM` the render thread's VIF1 time 4.78 s and 5.05 s to 2.61 s and
2.55 s; without it 5.91 s to 3.37 s and 3.35 s. What did it, by the host's measure:

| Change | Effect |
| --- | --- |
| The handlers' arithmetic fixes only an operand with the largest exponent (the thread flushes denormals and rounds toward zero) | VU1 −37% |
| A palette cache (256 entries, two ways), checked by `SLY1_GS_CHECK` | palette loads 95% from the cache |
| The transform's fast path unless a vertex field has the largest exponent | the transform 0.32 s to 0.12 s |
| A batch's positions and indices given to the GPU once, its colours per kick | 5.2 to 2.3 MB a frame |
| Unpacks specialised and done in place; VIF codes counted in an array | about 5% |
| Skinning, skinned normals, the lights and the outlines' projection on operands fixed once, then the host's arithmetic; conversions four fields at a time | skinning 4×, the outlines' own work halved |

Every change was checked by `SLY1_VU1_CHECK` (all 46 levels: 10,232,894 checks, 0 differed),
`SLY1_GS_CHECK`, `SLY1_UNPACK_CHECK` and the hashes above; the Switch build ran all 46 levels in
Ryujinx with `SLY1_GPU_TRANSFORM`.

Tried and not kept: a GS state change only on a changed value (the hash changed); dropping the
registers a deferred transform leaves (they reach the picture, `SLY1_NO_LEAVE`).

What remains, host profile of the render thread: unpacking 22% (2.3 MB a frame into VU1's memory),
the GS's handling of the kicked packets 15%, the second pass 12%, the outlines 8%, the deferred
transform's registers 8%, skinning and lighting about 25% together. The next large step is
lighting and skinning in the shaders.

## The destination alpha test (2026-11-17)

The game darkens what stands inside a volume (a level's dark boxes, the shadows) with the frame's
alpha as a mask: a volume's walls write alpha 0x80 where they are in front of the scene, then a
dark polygon is drawn with TEST's DATE where the alpha's top bit is clear (program 0x6DA's three
passes, and 0x076's shadow textures with DATM 1). Without the test the polygon darkens everything
seen through the volume: with the camera inside one (the opening's rooftop shots) the sky and the
buildings went black, on the Switch and on the host alike, for two reasons:

- **The GPU renderer did not make the test.** A GPU cannot test the frame it draws into, so the
  alpha bit is copied to the stencil (`BuildStencil`: the frame buffer's image drawn over the
  whole stencil with the alpha test keeping alpha 0x80 and more) before a draw with DATE, when a
  draw without it has come since; the draw then tests the stencil. The depth buffer is
  `ZF32_X24S8` for it. `SLY1_GPU_NO_DATE=1` leaves the test out, as before, for comparison.
- **The host's rasteriser truncated interpolated colours**, so the walls' constant 0x80 was
  written 0x7F and marked nothing. It rounds to the nearest now.

Found by leaving draws out of a frame of the host's rasteriser (temporary switches, not kept): the
frame was right without the untextured fans, then without only the DATE ones; counting pixels by
pass showed the walls' alpha written as 0x7F. In Ryujinx, lockstep, the rooftop shots are bright
with the test and dark with `SLY1_GPU_NO_DATE`. Its cost there: about 16 copies a frame (nearly
one a run with DATE, as other draws come between), the render thread's VIF1 time 2.6 s to 5.2 s
over 1,200 vblanks; to be measured on the Switch, and reduced if it shows (a copy only of the
part of the screen a run covers, or none when no draw since can have changed the bit).
