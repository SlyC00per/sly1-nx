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

- **The GPU renderer did not make the test.** Its pixel shader now makes it: for a run with DATE
  it reads the frame's alpha under the pixel (the frame buffer bound as a second texture while it
  is the target) and drops the pixel when the top bit is not DATM. A barrier comes before such a
  run when a run without DATE has come since, so that what was drawn is in the image when it is
  read. `SLY1_GPU_NO_DATE=1` leaves the test out, as before, for comparison.
- **The host's rasteriser truncated interpolated colours**, so the walls' constant 0x80 was
  written 0x7F and marked nothing. It rounds to the nearest now.

Found by leaving draws out of a frame of the host's rasteriser (temporary switches, not kept): the
frame was right without the untextured fans, then without only the DATE ones; counting pixels by
pass showed the walls' alpha written as 0x7F. In Ryujinx, lockstep, the rooftop shots are bright
with the test and dark with `SLY1_GPU_NO_DATE`.

What it costs, on the Switch (30 s from boot, the frame rate after the first 10 s):

| How the test is made | Frames a second |
| --- | --- |
| Not at all (the picture dark), depth buffer `ZF32` | 29.2 (before the opening's run was fixed: a different 30 s) |
| Not at all, depth buffer `ZF32_X24S8` | 24.3 |
| The alpha bit copied to a stencil before each run with DATE (about 13 copies a frame), behind a full barrier, a fragment barrier, or without unbinding the frame buffer | 16.4, 16.8, 16.8 |
| The same, each copy only of the run's box on the screen (43% of it on average) | 19.4 |
| In the pixel shader, depth buffer `ZF32` | 23.2 |

The stencil's cost went with the area copied: a depth buffer with a stencil is 8 bytes a pixel, and
a copy clears and fills it. Two ways with a stencil and no copies drew Sly black in the opening's
run and are not kept: the bit written to the stencil by the runs that mark pixels (a darkening
polygon also covers pixels its volume did not mark, where the frame's own bit decides), and the bit
guessed from each run's vertices (a texture's alpha takes part).

## The motion blur left out (2026-11-17)

The game ends each frame with a sprite textured from the other frame buffer (the frame before),
blended over the new one at FIX 0x30 (37.5%). A frame buffer is not a texture in the GPU renderer
(its image is the GPU's; textures are decoded from the GS's memory, which the GPU's drawing never
reaches), so the sprite drew that memory's black at 37.5% over every frame, and the whole picture
was darker than retail's. The run is left out (`SLY1_GPU_KEEP_BLUR=1` draws it as before, to
compare): the port has no motion blur, by the user's decision; if it ever has, as a setting that is
off. In Ryujinx six moments of the opening are then as bright as a recording of retail.

## Shadows' pictures left out (2026-11-17)

The game draws a 64 by 64 picture of a character into a buffer of its own (FBP 294) at the start
of a frame and then lays it on the ground under the character, as a texture, for its shadow
(program 0x076, with DATE). The GPU renderer does not draw into such buffers yet, so the texture
was whatever the GS's memory held at that address: the shadow's run laid it on the roofs as black,
all over with the vertex work on the CPU and now and then on the Switch (a roof flickering black).
A run textured from a buffer the game draws into and the renderer does not is left out
(`SLY1_GPU_KEEP_TARGET_TEXTURES=1` draws it as before): characters have no shadow until the
renderer draws into such buffers, which is the next step for them. Found with the vertex work on
the CPU (`SLY1_GPU_TRANSFORM=0`), where the roof was black in every frame: not the destination
alpha test, not the volumes' depths, not the depth test of the light's passes (each tried).
`SLY1_GPU_CAPTURE_ALPHA=1` makes a capture show the frame's alpha (green where its top bit is set).

## Small targets (2026-11-17)

The GPU renderer draws into the game's small buffers (64 to 256 pixels wide, 32 bits a pixel,
taken as square: the shadow's picture at FBP 294) and binds such a buffer's image where a run takes
it as its texture (`SLY1_GPU_NO_SMALL_TARGETS=1`: none, and those runs left out, as before). A
character's shadow is the case seen: its outline drawn from above with alpha 0x46, blurred, and laid
on the ground under it by program 0x076 with the destination alpha test.

- A buffer has two images of the game's own size. The blur is a run that reads the buffer it draws
  into: six sprites over the whole buffer, each a copy of it half a texel aside with the bilinear
  filter. Each sprite is drawn by itself from the current image into the other, which becomes the
  current one. (At four times the size the copies only moved the picture; at the game's size the
  filter spreads it as on the GS.) Any other run that reads its own target is left out.
- The target's size goes with the bound target: the vertices' scale, the viewport, the scissor. A
  small target has no depth buffer.
- Program 0x076 reads the positions of the batch the shadow falls on from VU1's memory, and the
  batch's header: 13.z, which the transform sets when a triangle reaches outside the guard band and
  the finish's clipper clears again unless a clipped polygon is left to draw. The late transform
  (`Materialise`) put the header back as it was, so with the vertex work in the shader the shadow's
  run took other triangles (a wedge of shadow on a roof, the shadow itself missing). It now keeps
  the flag as the transform and, when the batch has been kicked, the clipper (run with its kicks
  going nowhere) leave it, unless an upload has replaced the quadword since.

Ryujinx, lockstep, frame 560 of the opening: the shadow under Sly soft and translucent, and the
same with the vertex work in the shader and on the CPU. `SLY1_GPU_CAPTURE_SMALL=1` lays the first
small target over a capture's corner; `SLY1_GPU_SHADOW_TRACE=1` prints the first runs into a small
target, the first that read their own, and the first shadow runs' triangles.

### The shadow too dark: the picture never cleared

On the Switch the shadow came out nearly solid. The cause was the alpha under a blend. Each frame
the game clears the shadow's picture with a sprite of alpha 0 under blend 0x44 (Cs times As plus
Cd times one less As: nothing of the colour, and the alpha written 0), then draws the outline
with alpha 0x46 and blurs it. With the blend state's factors the GPU gives the frame's alpha the
colour's factor for what is there, whatever the state's own factors for alpha: the clear left the
picture as it was, and each frame's outline piled on the last's.

For a small target the pixel shader now does the blend's arithmetic (`coef` in gs_fsh.glsl): the
colour goes out times its own coefficient, the second output is one less the frame's, and the
second output's alpha is 1, so the alpha written is the source's, as on the GS. A small target is
also drawn half a pixel right and down: the GS takes a pixel's values at its corner, the GPU at
its centre, and which neighbours the blur's half-texel copies average goes by that.

The picture's alpha after the blur, counted over its 4,096 pixels at frame 501 of the opening
(`SLY1_GPU_CAPTURE_SMALL=1` prints `gpusmall:`), against the software GS:

| alpha | GPU | software GS |
| --- | --- | --- |
| 0x00 to 0x0F | 3,581 | 3,592 |
| 0x10 to 0x1F | 143 | 189 |
| 0x20 to 0x2F | 90 | 149 |
| 0x30 to 0x3F | 106 | 151 |
| 0x40 to 0x4F | 176 | 15 |
| 0x50 and over | 0 | 0 |

### Not done: the same for the frame, and what it showed

The frame's two buffers keep the blend state's factors. With the shader's form there too, the
dark volumes' first pass (alpha 0 over the volume's front faces, blend 0x6A) really clears the
frame's alpha, which it does not now, and the volumes work as on the GS. Two things then went
wrong, found at frame 620 of the opening under lockstep:

- In Ryujinx the third pass, which reads the frame's alpha for the destination alpha test, read
  it as it was before the second pass marked it (the sky above the roofs went black). A barrier
  before the read is not enough there; ending the command list and waiting for the GPU is, which
  is far too slow to do a dozen times a frame. Whether the Switch itself needs more than the
  barrier is not known.
- With the volumes working, Sly under the water tower is darkened, as the software GS draws him
  and as the retail video does not show him. So something of the volumes is still not understood
  (in the software GS too), and the picture the GPU renderer gives now, with the volumes mostly
  without effect, is the one closer to retail.

## Frame rate on the Switch: what limits it (2026-11-17)

Measured on a Switch Lite (handheld clocks) in the opening, 30 s after the first 10.

- The game overlaps its own update with the drawing: it builds one frame while the one before is
  drawn, and waits for the GS's FINISH of that one before it starts a third. So a frame takes as
  long as the render thread takes for it, rounded up to a vertical blank. (Telling the game
  FINISH when the drawing is queued was tried: the game's VIF1 interrupt handler, which packs
  colours from the frame's record while the frame is drawn, then ran on a record the game had
  taken back, and crashed.)
- `SLY1_GPU_AB=1` measures in one run: every 30 frames the next of six ways of drawing (as it
  is; the long pixel shader for every run; no destination alpha test; no batches; none of the
  GS's runs into the frame; everything cut to a sixteenth of the picture), and for each the mean
  frame time and the mean wait for the GPU. Without the GS's runs, or with a sixteenth of the
  picture, the wait is 0; without the batches it is as before. So the GPU's time goes to the
  GS's runs (sprites, particles, outlines), by the pixel.
- `SLY1_GPU_AREA=1` prints how many screens the GS's triangles cover each frame. The roof with
  the three steaming vents: 21 screens of blended, textured sprites a frame, at 960 by 720 where
  the GS drew 640 by 224.

What was done, each with captures of frames 501 to 506 and 620 to 625 under lockstep in Ryujinx
byte for byte as before:

- A character's outline is some 500 lines, each sent as two runs: the line, then the line again
  for its depth alone (ATST NEVER, AFAIL ZB_ONLY, the depth test GREATER) for the pixels the GS's
  antialiasing leaves without depth. Here the first run writes its depth everywhere, so the
  second can change nothing and is left out, and the first runs, now one after another with the
  same state, are put together into one draw (`SLY1_GPU_NO_MERGE=1`: neither).
- Three sets of a frame's memory in place of two, so that the render thread does not wait at a
  frame's end for the GPU to finish the frame before.
- A short pixel shader (gs_lite_fsh.glsl) for the GS's runs without the destination alpha test:
  no discard, none of the tests such a run never needs (`SLY1_GPU_NO_LITE=1`: the long one).

| build | frames a second | render thread waiting for the GPU |
| --- | --- | --- |
| before | 23.0 to 24.3 | 8 to 11 ms a frame |
| outlines as one draw, three sets | 24.5 | 7 ms |
| and the short pixel shader | 28.2 | under 4 ms |

Left: the render thread's own work, 21 ms a frame for VIF1's data (the VU1 handlers and their
kicks), which is now most of a frame.

## Small targets the game takes back (2026-11-17)

Reported from the run of all levels: from the second episode on, wrong textures, mostly on the
ground. A small target was kept for good under its frame base, and a run whose texture lay at that
base was given the target's image. A later level puts ordinary textures where an earlier one
drew a shadow's picture, and got the old picture for them; and once eight targets were made, a
further buffer was not drawn into and every run textured from its place was left out (11,794
runs into such buffers over the 46 levels).

A small target now remembers the count of uploads when it was last drawn into
(`gs::UploadSerial`). When a run takes its buffer as a texture and an upload has reached the
buffer's pages since (`gs::BufferGeneration`), the target is retired: the texture is the uploaded
one, and the target's images wait for the next buffer of their size. The same for a buffer this
renderer does not draw into. Over the 46 levels in Ryujinx: 35 targets retired, 5 in use at the
end, no run into a buffer left out. Level 20's first room, captured at full size
(`SLY1_GPU_CAPTURE_FULL=1`) in the run of all levels: the carpet had a stale picture across it
before, and is clean now.

Still open in that room: the window's glass is drawn far more opaque than the software GS draws
it (the city behind it hardly shows).

A capture's rows are printed 160 pixels a line: the emulator's log cut longer lines, so every
capture before this was missing its right-hand third.

## The GS on its own thread, with the batches (2026-11-17)

`SLY1_GS_THREAD=1` (the GS's work on core 1, after the render thread's VIF1 and VU1 on core 2)
drew nonsense once the renderer took batches: a batch went to the renderer at once from the
render thread, ahead of the GS's packets still queued. A batch is now queued with them
(`gs::KickBatch`, `gs::SetBatchSink`: its record and its vertices copied), the GS thread takes a
packet's loops in whole runs as the render thread does without it, and the render thread passes
its items on every 24, not once a frame, so that the two overlap within a frame. Ryujinx:
frames 501 to 506 and 620 to 625 under lockstep byte for byte as without the thread, and all 46
levels with it.

Not the default yet. `SLY1_PERF_LOG=1` prints, every 60 frames, the mean frame time and each
thread's share. The opening on a Switch Lite, the same stretches with and without the thread:

| frames | without: frame, render thread | with: frame, render thread, GS thread |
| --- | --- | --- |
| to 120 | 50.7 ms, 42.2 | 50.1 ms, 28.5, 29.6 |
| to 360 | 33.5 ms, 23.0 | 33.4 ms, 15.9, 14.3 |
| to 540 | 16.7 ms, 14.2 | 16.7 ms, 11.0, 6.1 |
| to 780 | 34.0 ms, 25.6 | 33.7 ms, 20.8, 17.7 |
| to 1140 | 59.5 ms, 46.6 | 51.5 ms, 31.8, 31.3 |

Each thread has less to do than the one did, but together they do more (the copies between
them), and a frame is done only when the GS thread is done with it, which is after the render
thread: in most stretches neither is yet short enough to gain a vertical blank.

## A sampling profiler on the Switch (2026-11-17)

`SLY1_SAMPLE=1`: a thread stops the render thread and the GS thread a thousand times a second
from the 12th second and notes each one's program counter (`svcGetThreadContext3`); the report
prints the counts by 64 bytes of code as offsets in the image, and
`python3 tools/sample_report.py LOG build-switch/sly1.elf` names the functions (the ELF must be
the build that ran). Ryujinx gives every sample the same place; it needs the hardware.

The render thread without the GS thread, the opening, of the samples where it was not waiting:
`gs::ImageRun` 11% (4-bit textures' uploads, a pixel at a time), the VIF's unpack 10%, GPU
driver calls 10%, then nothing over 6%: the leave-registers transform, `Vif::Words`, the
renderer's `DrawBatch` and `Submit`, the lit colours, `memmove`, a texture's lookup at each run
(its pages walked for the latest upload).

Done from it: an upload is written a block's row at a time (checked by `SLY1_GS_CHECK`: 0 image
runs differed over the 46 levels); a run with the texture of the run before, and no upload
since, takes that texture without the lookup.

## Lists with interrupts walked where they lie (2026-11-17)

The render thread drew every list of VIF1's from a copy made when the game started the chain. A
list can hold VIFcodes with the interrupt bit: VIF1 stops there, between two of VU1's programs,
and the game's handler (the decompilation's PackFrameColors) packs the colours VU1 has just lit
a mesh with into the mesh's cache, in the game's memory. The list then calls the cache's
packets. The copy had them as they were before the handler ran: at frame 848 of the opening
under lockstep, 24 meshes were drawn with no colour for a frame (72 runs of the counting
renderer's stream, against the same run without the render thread).

Such a list is now walked by the render thread where it lies, as VIF1 walks it on the PS2
(`SLY1_COPY_LISTS=1`: from its copy, as before); the others still go by their copies. The game
waits for a list's FINISH before it gives the frame's packets to anything else.

Compared frame by frame (`SLY1_GS_HASH_FRAMES=N` prints each frame's own hash; `SLY1_GS_HASH_RUNS=N`
a frame's runs) with `SLY1_RENDER_SYNC=1`, 899 frames of the opening, eight runs: all frames the
same but frame 40, where 12 runs differ by a depth step and a sixteenth of a pixel (a mesh's
matrix a tick newer, read after the game went on), and in two runs of the eight a stretch of 15
frames. Those stretches come and go from run to run, in the copies' day too (two runs of eight):
not explained.

Tried and taken out again: FINISH given to the game when a list is queued, so that the game
would compute the next frame while the list is drawn. It gained nothing (25.8 frames a second
against 25.4 on the Switch Lite), because the game does that already: its own render thread
waits for FINISH while its main thread builds the next frame, two frames' packets in turn. What
a frame costs is the longer of the two, to the next vertical blank. (`SLY1_PERF_LOG=1` shows
it: stretches at 16.7 ms a frame with the render thread at 14.)

Not found: the first frame of the opening's close shot of the police sign (frame 1426 under
lockstep in Ryujinx) lacks the building on the right, in every mode, without the render thread
too; retail's video has it from the first frame.

## A frame's list read in place, and the GS thread by default (2026-11-17)

`SLY1_TIMELINE=N` prints, from the Nth frame shown, the order of events with their times: what
the game's threads queue, what the render thread starts and ends, FINISH, the vertical blank,
the buffer swap, and the frame's semaphores. A heavy frame of the opening on a Switch Lite, from
its vertical blank: the game's render thread swaps the buffers and sends an upload (0.5 ms),
walks and copies the frame's list of VIF1's, 2 MB (7.5 ms); the port's render thread draws it
(35 ms); the game sends two more GIF packets, each with its FINISH, the first an upload of
200 KB (4 ms); it posts the frame done and waits for the next vertical blank: three blanks,
50 ms. The game's main thread builds the next frame meanwhile (it waits for a frame to be done,
with two frames' packets in turn), so it costs the frame nothing; the copy did.

A list of VIF1's of 64 KiB or more is no longer copied. Its tags are walked for its length, and
the render thread walks it where it lies, giving the VIF the quadwords that follow each other in
memory as one stretch (a quadword at a time cost the render thread 5 ms a frame). A shorter list
is copied, unless it has interrupts. `SLY1_COPY_LISTS=1`: every list copied, as before.

The GS thread is on by default on the Switch (`SLY1_GS_THREAD=0`: not).

The opening on the Switch Lite, 40 s after the first 10, `SLY1_PERF_LOG=1` (which now also gives
the time any of the game's threads ran):

| build | frames a second | a heavy stretch: frame, render thread, GS thread, the game's |
| --- | --- | --- |
| lists copied | 24.7 | 62.3 ms, 46.6, none, about 20 |
| lists in place | 26.2 | 50.6 ms, 46.0, none, 9.8 |
| and the GS thread | 27.0 | 50.0 ms, 32.9, 25.8, 9.7 |

Frames of four vertical blanks after the 600th: 132, then 11, then none.

Verified as the change before: 899 frames against the run without the render thread, all the
same but frame 40 (and the stretches of 15 that come and go); the Mac's --verify-levels with
both checks and Ryujinx's, with and without the GS thread, all 46 levels; Ryujinx's captures of
frames 501 to 506 and 620 to 625 byte for byte as before.

### The upload after each frame, at the opening's end

The 200 KB the game uploads after each frame's list (4 ms of the render thread on the Switch) is
particular to the scene with the title: a 512 by 512 4-bit picture at DBP 5120 and others, into
the pages where the textures uploaded before the list lie (1,024 by 128 at DBP 5248, two of 128
by 128). Each set writes over the other every frame, so neither is "the same as last time" and
both are written (some 280 KB a frame). Elsewhere in the opening the set before the list is
found unchanged and skipped. A fix would keep an upload's data and write it into local memory
only when something reads those pages; not done, since it is one scene.

## How much of a frame the frame before had (2026-11-17)

`SLY1_GS_REUSE=1` with the counting renderer taking batches (`SLY1_GS_COUNT=1
SLY1_GS_COUNT_BATCHES=1`): of a frame's batches, how many are the same as a batch of the frame
before, by a hash of their vertices. The opening under lockstep, 3,000 frames, by stretches of 240:

| the same in | least | most |
| --- | --- | --- |
| x y z, the control words, S and T | 84.1% | 95.5% |
| the colours | 88.2% | 99.0% |
| all of those | 81.5% | 94.4% |
| the matrix | 0.0% | 18.7% |

A batch's vertices are as VU1 was given them (the renderer transforms them), so a mesh that does
not change is the same whatever the camera does: four batches in five to nineteen in twenty are
drawn again from the same vertices and colours, with another matrix. (Q and the word after it
are left out of the hash: they hold what was last in VU1's memory there, and with them only one
batch in ten matched.)

Every one of those is unpacked by the VIF, taken through VU1's handlers, copied to the GS thread
and copied to the GPU's memory again each frame. That is the work a renderer that kept a
batch's vertices from one frame to the next would not do.

## A hook: the model part drawn from its light cache (2026-11-17)

The user's decision: the port may add named hooks to the game's mesh-drawing code, through
tools/patches.py (the decompilation is not touched).

The first. Where the game draws a model part whose lighting has not changed, it sends the part's
header (BASE, OFFSET, an unpack of 11 quadwords: the matrix and what goes with it) and then
calls, with a DMA call tag, the packet its light cache holds: the part's meshes with their
colours as they were last lit. The patch puts a VIF NOP before the call, in the word the game
pads the header's tag with (the tag is no longer), with the packet's address and which of the
two it is in the NOP's unused bits. The render thread, walking the list where it lies, sees the
NOP and then the call, and the tag that returns from the call: the batches kicked between are
the part's (`gs::PartOpen`).

The opening under lockstep on the Mac, 3,000 frames, the counting renderer taking batches:
740,142 parts (247 a frame) with 4,838,364 batches in them and 1,090,510 outside: 82% of the
batches, 79% of the vertices. The counting renderer's stream is as before (899 frames against the
run without the render thread: frame 40 alone differs, as before).

What it is for: a part's packet is the same from frame to frame until the part is lit again, so
what the renderer made of it (each batch's vertices and colours, and the GS registers written
between them) can be kept and drawn again with the new header, without the VIF's unpacks, VU1's
handlers, the copy to the GS thread or the copy to the GPU. Not built yet.

## A model part drawn again from what was kept of its packet (2026-11-17)

`SLY1_PART_REPLAY=1`, with the GS thread; off by default. Built on the hook above.

The first time a part's packet is called, what the part queues for the GS thread is kept: the
packets that set registers and the batches (their vertices as VIF1 unpacked them, for the
renderer to transform). After that, while the part is what it was, the render thread does not
follow the call: it queues the kept packets and batches again, the batches with the matrix made
of this frame's header and VU1's matrix at 4 (`vu1::PartMatrix`, the arithmetic of the program
at 0x31B), and sets VIF1's registers as the packet left them. Neither VIF1's unpacks nor VU1's
programs run for the part.

"What it was" is three things:

- VIF1's registers when the call comes.
- The header, in what the packet's programs read of it apart from the matrix: quadwords 8 and 9
  and 10's w. (Quadwords 4 to 7, the inverse matrix, are for lights the cached path does not
  have. Quadword 10's x, y and z are whatever was on the game's stack: compared, they made a
  quarter of the parts look changed every frame.)
- A hash of the packet as VIF1 would take it (`PartHasher`): its tags, every VIFcode, and every
  code's data whole but for unpacks of more than 64 words, the vertices, of which one quadword in
  eight is taken. A part is hashed whole, vertices and all, the first eight times it is drawn
  again and one frame in 32 after; one found changed where the eighths were not is marked as
  moving and run as ever from then on.

A part is not kept when anything else came of it: a batch not handed over as unpacked, a batch
whose matrix is not the header's times VU1's, a packet with a vertex's registers in it, an
interrupt, a FINISH. Everything kept is let go when it passes 48 MB.

What the comparison with the run without it found on the way (the counting renderer's hash of
each frame, which for a batch now takes what is drawn of a vertex and not Q and the word after
it): a flag whose vertices move while its packet does not (hence the whole hash); a texture's
place, which the game writes anew in a mesh's data when it moves the texture, missed for up to 32
frames while the pieces a tag refers to were only sampled (hence the hash by VIFcodes); and the
stack's words in the header.

Verified. The Mac, 899 frames of the opening under lockstep, the counting renderer taking batches
with the GS thread: three runs with it against a run without, every frame's hash the same (one
further run differed in a stretch of 15 frames, the stretches that come and go). 164,873 parts
drawn again of some 180,000, 10,251 kept anew; the render thread's time for VIF1's data 1.54 s
against 1.94 s, 0.30 s of it the hashing; 34 MB kept. Ryujinx with the GPU renderer: frames 501
to 506 and 620 to 625 byte for byte as without it; --verify-levels with it printed "all 46
levels loaded and ran 60 frames" (586,918 parts drawn again, no batch refused).

Not yet: the renderer still copies a kept batch's vertices to the GPU's memory every frame, and
the hash is a walk of each part's packet every frame, which a hook where the game writes a
cache's packet would spare.

### On the Switch, and the two things it needed

The first build with `SLY1_PART_REPLAY=1` gained nothing on a Switch Lite (26.6 frames a second
against 27.0), though 279,019 parts were drawn again. The render thread queued each kept packet
and batch as an item of its own, some 3,000 a frame, each with memory of its own: that cost as
much there as running the parts. A part drawn again is now one item, and the GS thread goes
through what was kept. With that: 29.7 frames a second, the render thread 15 to 25 ms a frame
where it took 17 to 33, and frames of three vertical blanks after the 600th down from 250 to 100.

The GS thread was then the longer of the two (16 to 31 ms), and `SLY1_SAMPLE=1` showed 22% of its
time in the renderer's `DrawBatch`, copying each batch's vertices to the GPU's memory. So:

- A kept batch has a key (`gs::Batch::keep`), the same for as long as its vertices and colours
  are. The Switch's renderer writes such a batch's indices, positions and STQ, and colours once,
  into memory of their own (`keepMem_`, 64 MB), and draws from there each frame after. The render
  thread tells it, in turn with what is queued, when a record's batches will not come again
  (`gs::ForgetKept`); a block let go is taken again three frames later. When a batch finds no
  room, everything is let go at the frame's end and written again as it comes.
  `SLY1_GPU_NO_KEEP=1`: copied each frame, as before.
- A part found changed three times running (one that fades: its header's scales are other each
  frame) is run as ever for the next 30 frames, and one that could not be kept is not tried
  again for 30.

Verified: the Mac's 899 frames with the counting renderer, every frame's hash as without it;
Ryujinx's captures of frames 501 to 506 and 620 to 625 byte for byte as without it (975,456
draws from 9,534 batches written once); Ryujinx's --verify-levels with it, "all 46 levels loaded
and ran 60 frames" (4,336,729 draws from 152,932 written, the memory emptied once, the GS
thread's time 22 s where it was 40.5 s).

### On by default (2026-11-17)

With the batches kept on the GPU the opening on the Switch Lite holds 29.9 frames a second: two
vertical blanks a frame nearly throughout (741 frames of two after the 600th, 85 of three, none
of four), the render thread 14 to 16 ms a frame in most stretches and the GS thread 12 to 21.
The user found the picture right, and it is on wherever the GS thread is
(`SLY1_PART_REPLAY=0`: not). The hash takes two words at a time where it takes every word.

`SLY1_SAMPLE=1` in that run. The render thread, of the time it was not waiting: the parts' hash
15%, the lit colours and the lights 14%, the unpacks 6%, the registers left by the transform 5%,
the chain's walk 5%, the outlines 4%. The GS thread: waiting on the GPU's driver 28% of its busy
time, the uploads 12% (4-bit textures the game uploads every frame), `DrawBatch` 9%. With the
parts drawn again, what VU1's handlers still do (the Mac, `SLY1_VU1_PROFILE=1`) is what changes
every frame: the characters' outlines first, then skinned and newly lit meshes, sprites,
particles.

A frame is two vertical blanks because its stages come one after another: the list's hand-over,
the render thread's 15 ms, the GS thread's tail and its wait for the GPU, the game's packets
after the list. For one blank they must come to under 16.7 ms together.

### The stages' fixed costs (2026-11-17)

- An upload that fills whole pages of local memory, and that has been written before with the
  same bytes, has its pages put back as that writing left them (`gs::ImageRun`; 16 MB of such
  pages kept), in place of its pixels one by one. The scene with the title uploads two sets of
  textures over each other every frame (above), 280 KB written a frame. `SLY1_UPLOAD_VERIFY=1`
  writes all the same and compares: over 3,000 frames of the opening 6,313 uploads were put
  back, none of them other than writing gave; the same over the 46 levels.
- The parts' hash takes one quadword in 32 of the vertices (it took one in eight): the whole
  hash is what finds vertices that move.
- The walk that finds a list's length counts a tag's data without going through it.

## The GPU's time: the batches' pixels (2026-11-17)

With the parts drawn again the GS thread waited 10 ms a frame for the GPU (14.7 s of a 50 s run
on the Switch Lite). `SLY1_GPU_AB=1`, its six ways changed for the question: the destination
alpha test without its barrier, nothing drawn into small targets, and no runs with the test
each left the wait as it was (7 to 11 ms); no batches, or a sixteenth of the picture, took it to
nothing. So it was the batches' pixels. Two things about how a batch was drawn:

- Its backfaces were dropped in the pixel shader (`discard`, by a flag on the triangle's last
  vertex), so the GPU could leave out no hidden pixel before running the shader. Where every
  triangle of a batch is one to drop when it faces away (all but a few are), the rasteriser now
  culls, and the batch has the short pixel shader, which has no discard. `SLY1_GPU_NO_CULL=1`:
  as before. Not the same to the pixel: in Ryujinx, frame 620 of the opening at full size, 49
  pixels of 691,200 differ, single pixels along edges (a triangle seen edge-on is not taken the
  same way by the two).
- The game tests nearly everything against an alpha of 126, with FB_ONLY for what fails: two
  draws of every such run, what passes and what fails. Where the alpha is known to lie on one
  side for the whole run (the texture's least and greatest alpha, kept when it is decoded, with
  the vertices' for a batch), there is one draw, without the test. Also where what fails goes to
  the frame all the same and nothing writes the depth. `SLY1_GPU_NO_ALPHA_RANGE=1`: as before.
  The same to the pixel (frames 501, 620 and 2,700 and the five after each).

The second `SLY1_GPU_AB=1` (as it is; backfaces in the shader; every test in two draws; both):
the wait for the GPU 0.00, 10.17, 4.16 and 10.97 ms a frame, the frame 24.3, 32.2, 26.7 and
35.5 ms. Then the opening as it is: 41.7 frames a second where it was 30.0; of the frames after
the 600th, 798 of one vertical blank, 624 of two, 2 of three; the wait for the GPU 3.0 s of the
run, nearly all of it on the roof with the steaming vents (21 screens of blended sprites a
frame, each drawn once already), where the GS thread takes 26 to 31 ms a frame.

### The translucent effects at half size

What was left of the wait for the GPU was the steam: about 21 screens of blended sprites a frame
on the roof with the vents. Those runs now go into a picture of their own at half the frame's
size each way (480 by 360), which is laid over the frame, added to it, before anything else is
drawn into the frame: a quarter of the pixels, and the steam slightly softer (accepted).

- What goes there (`Renderer::FxRun`): the GS's own triangles into a frame buffer, blended as Cs
  times As or FIX plus Cd (ALPHA 0x48 or 0x68: added to what is there, so the order among them
  and against the frame does not matter), that write no depth: the alpha test NEVER, or one no
  pixel of the run can pass (`AlphaDecided`), with what fails drawn to the frame. Not a batch,
  not a run with the destination alpha test, not one into a small target.
- The half-size picture has no depth buffer. The depth test is in its pixel shader
  (`gs_fx_fsh.glsl`), against the frame's depth buffer read as a texture: a pixel stands for four
  of the frame's and is drawn where the run is in front of the nearest of the four.
- What those runs would write to the frame's alpha is not written.
- `SLY1_GPU_NO_FX=1`: as before.

Checked in Ryujinx against `SLY1_GPU_NO_FX=1` under lockstep, at a quarter size: every 100th
frame of the opening from 420 to 2,720 and frames 750 to 850 by twenties (the dust behind the
falling bricks). One pixel in all (frame 720) differs by more than 24 of 255. (A first version lost its depth test
to a later line that set the same shader field: smoke showed through a wall.)

On the Switch (the Lite, handheld, the clock as it is; measured with that first version, whose
runs were not depth-tested), the opening: 44.0 frames a second where
it was 41.7; of the frames after the 600th, 929 of one vertical blank and 614 of two; the wait
for the GPU 0.94 s of the run where it was 3.0 s. The roof with the vents: 25 to 26 ms a frame.
What is left at two blanks is the processor's: the render thread and the GS thread, 15 to 21 ms
each in the busy stretches.

### The GS thread's own overheads (2026-11-17)

`SLY1_SAMPLE_FRAMES=A,B` limits the sampler to the frames shown from A to B. Frames 1,500 to
1,900 of the opening (two vertical blanks a frame; both threads busy about 18 ms of 33): no one
function stands out on either thread. On the render thread the characters' lighting, skinning
and outlines are about 5 ms, the parts' hashes about 2. On the GS thread, by source line, a
tenth was the first read of each kept item of a part drawn again (its words behind three
pointers), 7% the lock (taken twice an item) and 6% the map of kept batches. Now the GS thread
takes everything queued at once and gives its emptied buffers back then; a kept part's items
carry their words' address and the next one's memory is asked for ahead; the kept batches are in
one flat table (`KeptTable`). The same output (the host's hash a frame over 898 frames; six
frames in Ryujinx to the byte).

On the Switch, frames 1,500 to 1,900: the GS thread 12.1 to 17.0 ms a frame where it was 14.3
to 20.0; the opening's frames after the 600th, 1,013 of one vertical blank and 574 of two
(948 and 605). The render thread, 15 to 21 ms there, is what holds those frames at two.

### The handheld GPU clock: not used

`SLY1_PERF_CONFIG=92220007` asks the system for that performance configuration in handheld mode
and prints what it had and has (it had 00020003 and took it). With it the opening went from 41.7
to 44.2 frames a second on the Switch Lite and the roof with the steam from 43 and 34 ms a frame
to 33 and 27. It is a trial switch and stays off: the port is to reach its frame rate at the
system's default clocks (the user's decision: less power and heat are what the port is for).
