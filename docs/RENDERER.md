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

### A volume's marks written (2026-11-18)

A searchlight in "Back Alley Heist" was not drawn as the light where its cone meets the walls
and floor: nothing on the Switch, the whole cone faintly in Ryujinx. Program 0x6DA draws a
volume in passes: the faces of one side write alpha 0 where they are in front of the scene
(TEST 0x71001: the alpha test never passes and its failure writes the frame alone; the depth
test is GREATER), the faces of the other side write 0x80 again, both with blend 0x6A, which
leaves the colour as it is (Cd); then the tint (yellow, alpha 0x41, blend 0x48) is drawn with
the destination alpha test where the top bit is clear, which is where a surface of the scene
lies inside the cone.

With the blend on, the GPU gave the frame's alpha the colour's factors, so the two marking
passes wrote nothing. A blend that leaves the colour as it is is now drawn without a blend and
with the colour not written (`alphaOnly` in `Renderer::Submit`; `SLY1_GPU_NO_ALPHA_MARK=1`: as
before). Seen right on the Switch. Ryujinx shows the whole cone with and without it: it reads
the frame's alpha as it was before the passes, as noted above, so this cannot be checked there.

The other blends still give the frame's alpha the colour's factors (the section above), and
the dark volumes of the opening mark the frame now: whether Sly under the water tower is
darkened again has to be looked at on the Switch.

To look at a place on the host or in Ryujinx: `SLY1_PERF_LOG=1` prints the player's position
each second (`perfpos:`), and `--level N --env SLY1_WARP=x,y,z[,frames]` places the player
there with the game's own SetPlayerPosition once the level has run (`SLY1_LIGHTS=1` lists the
level's lights). The spot here: `--level 20 --env SLY1_WARP=2899,-2340,2080,600`.

## Widescreen: the world (2026-11-18)

The picture is the whole 16:9 screen: a frame buffer's image is 1,280 by 720 (it was 960 by 720
in the screen's middle), the game's 640 columns across it, and the camera's horizontal extent
over the vertical is 4/3 of the PS2's (`port::WideFactor`, in tools/patches.py's patch of
`InitCamera`'s aspect, from which the projection, the frustum's sides and what they cull all
come). The vertical field of view is the PS2's. `SLY1_WIDE=0` keeps the PS2's aspect (the
picture is then stretched: for comparison only).

Seen on the host (a frame at a place in "Back Alley Heist", stretched to 16:9: nothing missing
at the sides), in Ryujinx and on the Switch (played: nothing coming or going at the edges).
The map page (shown when a level is left) is an object laid before the camera to fill the 4:3
picture: it is made 4/3 wider across the screen (a patch of `ApplyWarpPageTransform`), as the
PS3 release stretches it; before, the level showed at its sides. `--level N --env SLY1_MAP=1`
opens a level's map page on the host.

The 2D elements are laid out for the 4:3 picture. Each screen element (`DrawScreenElements`'s
loop) is drawn 3/4 as wide about the left edge, the middle or the right edge of the screen, by
the third in which the middle of its resting place lies (`port::Wide2D`), so it has its shape
and keeps its side; a vertex at the screen's very edge stays there, so a fill of the whole
screen (a fade, the letterbox bars) still spans it. Every 2D vertex goes through four functions
of the game (`AddGifXyzEntry`, `AddGifXyzfEntry`, `AddGifXyzfNoDrawEntry`,
`AddGifXyzfRegListEntry`), patched to pass its x through `port::WideX`. Seen on the host (the
map page's caption) and on the Switch (the user's report: the counters, the pause menu, the
text, the binocucom and the markers all right).

Not done: videos (none are played yet). 2D drawn outside a screen element is as wide as the
picture makes it; none has been reported. On the Switch the same place costs more
than at 4:3 (a third more pixels and more of the world in view): the run's median frame was
33 ms where the 4:3 run's was 28.

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

In that room the window's glass was then drawn far more opaque than the software GS draws it
(the city behind it hardly showed). No longer, as of 78c8829 (2026-11-17): captured again at
full size (`--levels 20`, the run's last frames), the city shows through the glass as in the
host's dump of the same room. Which of the changes since did it was not traced.

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

### The parts' hashes: three things that gained nothing

The hash of the parts drawn again is about 2 ms a frame of the render thread in that stretch,
by line the first read of each small piece of a packet and the short runs after it. Tried on
the Switch, each with the same output, none with a gain (kept out):

- memory asked for a few lines ahead inside a run, and the next referred block's first lines;
- long runs summed in four independent sums, and a sampled run's places asked for up front
  (the runs are mostly under 32 words);
- each part's lines of memory listed once and asked for when the part before it is reached:
  the hash 18% cheaper, the asking and its lookups as dear as what it saved.

### The parts' hashes a frame late, on a helper thread (2026-11-17)

What did gain: the hash is no longer made before the decision. A kept part is drawn again on
what a helper thread found the last time it was asked (the frame before, as a rule) and the
helper is asked again (`HashHelperMain`, `PartHashLate` in port/platform/dmac.cpp). A part whose
packet has changed is drawn once more as it was and kept anew the frame after; the registers
and the header are still compared at once. A part found changed is hashed before each decision,
as before, for the next 120 frames: without that a part that changes every other frame was
drawn as it was every other frame for good (on the host, every second frame's hash differed).
`SLY1_PART_HASH_NOW=1`: every hash on the render thread, as before. The picture is no longer the
same to the frame (the user's decision, 2026-11-17: frame-exact is not needed where it looks
the same): on the host 15 of 898 frames of the opening differ in their hash, one part a frame
late in each.

The helper's place, found the hard way:

- The fourth core is not the program's (`svcSetThreadCoreMask` refuses it); the helper is on
  the game's core, which the game's threads leave about half idle.
- A thread cannot be made from the render thread: `std::thread` never returned there (the
  Switch showed its error screen; Ryujinx stood still at frame 21). The program starts the
  helper beside the GS thread.
- At the game's threads' priority it held them up: frames that had just made a vertical blank
  missed it (825 of the opening's frames after the 600th in one blank, where 1,013 had been).
  On the GS thread's core it gained nothing. Below the game's threads (0x2D) it runs only in
  what they leave; the render thread takes the lock it shares with the helper only when it is
  free, since the helper may be set aside while it holds it.

On the Switch, the opening's frames after the 600th: 1,290 of one vertical blank and 434 of
two (1,013 and 574). The render thread in frames 1,560 to 1,800: 18.0 to 19.2 ms a frame where
it was 20.1 to 20.6.

### Smaller cuts after the helper (2026-11-17)

Each with the same output (the host's hash a frame over 898 frames with `SLY1_PART_HASH_NOW=1`;
six frames in Ryujinx to the byte), measured on the Switch over frames 1,560 to 1,860:

- DIRECT's data for the GS thread copied in whole runs, not a quadword at a time: no gain that
  shows (the render thread's `memmove` and `Vif::Words` as before). Kept: it is the simpler path.
- The GS held once for each of the GS thread's items, not once for each packet and batch in it;
  the place of the next kept batch in `KeptTable` asked for a turn ahead (`Backend::KeptAhead`);
  the last 64 textures found kept before the map (`Renderer::recent_`). The GS thread 14.3 to
  15.8 ms a frame where it was 15.0 to 16.6.

The opening's frames after the 600th: 1,317 of one vertical blank, 421 of two. What is left at
two blanks is frames 1,560 to 1,860, where the render thread is busy 17.7 to 19.0 ms a frame:
`LitColours` 2.2 ms, `LightFourWide` 1.2, the unpacks 1.2, `memmove` 1.0, `BatchTransformNow`
0.9 (the registers a deferred batch leaves), the list's walk and words 1.7, outlines 0.7.

### The parts drawn the long way (2026-11-18)

A part with no light cache to draw from (`DrawModelPart`'s slow path: the game writes the
part's own packets, and VU1 lights it each frame) had no hook, so it was never drawn again
from what was kept. A second patch in `tools/patches.py` makes that path a chain of its own: a
marker (two NOPs, `0x000051A0` and `0x00800000 | cache >> 4`), a CALL, a NEXT over the chain,
the part's packets, a RET. `PartCall` takes it as it takes the first hook's, with these
differences:

- The header is unpacked inside the chain, so it is read from the chain's first tag (BASE,
  OFFSET, the unpack of 11 quadwords) and left out of the hash; when the chain is not followed
  it is written where the unpack would have put it.
- The same cache is drawn more than once in a list, so a part's key has its turn in the list
  (`g_partTurns`) and a bit for the long way.
- VU1 lights such a part from the header, so the header is compared whole from quadword 4 on
  (the inverse matrix and the light's centre with the rest). Comparing only what the cached
  parts compare kept the colours of an earlier frame while the part or the camera turned: 693
  of the opening's 898 frames hashed differently on the host. With the whole compare all 898
  are the same as with `SLY1_PART_REPLAY=0` (`SLY1_PART_HASH_NOW=1`).
- The helper's verdict may come too late for a part first seen this list; one with no verdict
  for more than four lists is hashed on the render thread.

`SLY1_PART_SLOW=0` runs such parts as ever. What it gains is small where things move: in the
opening 689 such parts are drawn again (5,432 with the looser compare), and on
the host's 360 frames at a level's start "Into the Machine" has 70 (9,413 before the compare
was made whole), "Piranha Lake" 14,121 (37,513), "A Ghastly Voyage" 3,551 (23,371), "Treasure in the
Depths" 18,849 (21,312). The parts refused are refused rightly: their inverse matrix does
change. Drawing them again would need the lighting done apart from the geometry.

A census of the levels is easy to misread. `--verify-levels` with `SLY1_LEVEL_FRAMES=360`
stands at each level's start, and several levels change to a far lighter scene after some
seconds ("Back Alley Heist": 580 parts drawn again a frame, then 40, and the frame from 33 ms
to 17). Whether that moment falls inside the 360 frames differs from run to run, so a level's
mean says little: three levels that looked slower with this patch were only standing in the
heavy scene for longer. `SLY1_PERF_LOG=1` now prints the parts' counters a frame beside each
second's times (`perfparts:`), and `partcounts:` after each level; compare seconds with the
same counts. `SLY1_ADDR_TRACE=1` prints each caller that turns an unmapped game address into
a pointer (none in any level).

### What still changed each frame in a still scene (2026-11-18)

"Back Alley Heist" from its start, nothing moving: 580 parts drawn again a frame, 47 kept
anew and 140 run as ever. `SLY1_PART_DIFF=1` (with `SLY1_PART_HASH_NOW=1`) hashes each part's
packet a VIFcode at a time and says what differs from the list before, and why a part is not
kept (`partdiff:` after each level). It found:

- **A texture's place.** 41 parts a frame differed in one quadword: the TEX0 write among the
  registers the packet unpacks for VU1 to kick, its TBP0 and CBP other each frame (the game
  places textures anew in the GS's memory). The hash now leaves the value of such a write out,
  the part's record notes where the writes are and where each is in the kept packets, and a
  part whose textures have moved is drawn again with the kept packets' TEX0 writes replaced
  (`texture moved` in `partcounts:`). Exact: the opening's 898 frames hash the same.
- **The alpha's scale.** 52 parts a frame had 9.x other in its last digits (a fade with the
  distance: 0.65348, then 0.653482). Within 1/512 of what the part was kept with it is drawn
  as kept. Not exact, and off under `SLY1_PART_HASH_NOW=1`.
- **The turns' table.** `g_partTurns` was a map emptied for each list: a node allocated for
  each part of each list, over half of the render thread's allocations. A fixed table now.
- **Parts drawn the long way that are unlit or lit by their cache** (mode -1 or 0) read
  neither the inverse matrix nor the centre: compared as the cached parts are.

After it: 675 drawn again, 7 kept anew, 84 run as ever. On the Switch that scene went from
21.5 to 20.6 ms a frame of the render thread: the parts that changed were not where its time
is. `perfparts:` now has the time for parts drawn again and for parts run in full; on the
host the 90 run in full are 63% of the render thread's time and the 675 drawn again 13%.
Those 84 are: 34 lit by VU1 afresh (mode 4: skinned, or with a light of their own), 33 that
are never kept because their packets write vertices (register 1, RGBAQ: triangles the
clipper cut, which depend on the matrix), 8 with a program the handlers do not take, and
parts that do fade.

### Parts run beside the render thread (2026-11-18)

What was left of the render thread's time in a heavy scene was the parts that cannot be drawn
again from what was kept: those VU1 lights afresh each frame (skinned, or with a light of their
own), those whose packets write vertices (outlines), those that fade. In "Back Alley Heist"
they were 16.9 of the render thread's 24.7 ms a frame on the Switch, the 624 parts drawn again
2.6. Such a part is now a job (`PartJob` in port/platform/dmac.cpp; `SLY1_PART_JOBS=0` turns
it off):

- The part's packet is walked through a VIF1 of the job's thread into a VU1 of its own
  (`vu1::Lane`: registers, data memory, the deferred batch's record), begun as a copy of the
  render thread's, with the micro memory and its table of translated chunks as they were when
  the job was made (`MicroNow`: the render thread loads other microcode further down its list).
  What the part queues for the GS is collected in the job.
- The render thread goes on at once from the registers the part left the last time it ran
  there. The packet's shape answers for them (`PartHash`'s shape: its VIFcodes and tags, and
  the data of the codes that set VIF1's registers); a shape with microcode loaded or an
  interrupt is never a job. A part comes with either of two sets of registers (the game turns
  VIF1's buffers about from part to part), so a record holds two pairs; a part met with
  registers it has no pair for is run in its turn and watched for the pair.
- An item in the GS's queue stands for the job, and the GS thread takes the job's items there,
  where they are. The helper thread (core 0, below the game's threads) takes jobs in their
  order; the render thread takes the rest when its list is walked and does not begin the next
  list before every job is done (a part's packet may lie in the frame's own memory).
- A thread that needs a job another has begun and not ended within 0.4 ms runs it again itself
  and does not wait: the helper is set aside by the game's threads in the middle of a job for
  as long as they run. Each thread works on its own copy of the job's lane, and the first to
  end gives the job its items (`given`; `gen` and `runners` keep a thread set aside in an old
  job from giving it anything once the job is another part's).
- A job's thread takes no lock and allocates nothing: the items stay in the job from one use to
  the next and each one's buffer is used again; the thread's VIF1 is kept. With a lock (the
  buffers' stock, the allocator's) the helper was set aside holding it, and jobs on the GS
  thread took 1.75 ms each.
- The helper thread has the game's float mode (`SetGameFloatMode`). Without it the parts it ran
  were rounded to nearest and could overflow to infinity: Sly's shadow flickered.

Also: a part that keeps changing is left alone for twice as long each time (30 frames to 960)
until it has been drawn again 60 times from one keeping. Keeping a part costs several times
running it, and a part that fades for seconds was kept anew three times a second.

Checks, on the host: `SLY1_PART_JOBS_CHECK=1` runs each job at once on the render thread too
and compares what the job's thread gives, word for word: 146,236 jobs over the 46 levels, 0
differed. `SLY1_PART_HASH_NOW=1 SLY1_PART_HELPER=1` runs the helper for the jobs alone with
every hash before its decision: the opening's 898 frames hash as with `SLY1_PART_REPLAY=0`,
but for a burst of 15 frames in some runs in which one full-screen pass at the frame's end has
its T coordinate half a line other (the field the frame falls on, which the host's timing
moves; not the jobs). `SLY1_PART_JOBS=now` runs each job at once (the same work, no other
thread); `SLY1_PART_JOBS_ONLY=slow|cached|N` makes jobs of one kind of part.

On the Switch, "Back Alley Heist" played from its start: 52.5 frames a second over the first
minute (46.8 with the first version of the jobs; a median frame of 33 ms with
`SLY1_PART_JOBS=0`, 50 ms with `SLY1_PART_REPLAY=0` too). Further in, the render thread is at
13 to 16 ms, the GS thread 10 to 12, the game's threads 10 to 14, and most frames still take
two vertical blanks. Jobs the GS thread runs itself take it 0.95 ms each (the helper 0.16, the
render thread 0.12): not understood yet.

Not from this: a searchlight in that level is not drawn at all, with the jobs and the parts'
reuse both off as well. Likely one of the volumes that mark the frame's alpha, which the GPU
renderer leaves without effect ("Not done", above). And once, at a start, the game began a
chain on channel 0x4A8E3020 and faulted before any level was loaded; the next start of the
same program ran.

### The handheld GPU clock: not used

`SLY1_PERF_CONFIG=92220007` asks the system for that performance configuration in handheld mode
and prints what it had and has (it had 00020003 and took it). With it the opening went from 41.7
to 44.2 frames a second on the Switch Lite and the roof with the steam from 43 and 34 ms a frame
to 33 and 27. It is a trial switch and stays off: the port is to reach its frame rate at the
system's default clocks (the user's decision: less power and heat are what the port is for).

## A building one frame late after a cut: the game's own order (2026-11-17)

In the opening, on the first frame of the shot of the "Le Police" sign, the building to the
sign's right is not drawn; it is from the second frame on. Not the renderer's: the host's
software GS shows the same with `SLY1_RENDER_SYNC=1`, and the game does not queue those parts
that frame (51 parts of the world and 10 of six objects, queued from the next frame on).

The cause is the order of the game's main loop (`RunMainLoop`): `UpdateCameraLocation` looks up
the area the camera is in (`Camera::areaMask`, from the world's value tree at the camera's
position) at the start of the frame, before the world's frame moves the camera; the parts are
then queued for the camera's new position against the old area's mask. Traced on the host: at
the cut the lookup still has the last shot's position (the mask stays 0x400), and the frame
after it has the new one (mask 1). Retail runs the same code in the same order on one thread,
so it leaves the building out for that frame too: a sixtieth of a second there, twice as long
where the port takes two vertical blanks a frame. Left as it is (the port's gameplay is
retail's); it shortens with the frame.


## Particle streaks in native code (2026-11-19)

`SLY1_SAMPLE=1` on the Switch in A Stealthy Approach (a minute of play, 9c1f6cf): of the render
thread's samples that were not waiting, 47% were in one translated chunk of microcode,
`chunk_000_19FE9C91`. `SLY1_VU1_PROFILE=1` on the host names the entry: 0x073 of set B's first
chunk, 97.5% of the instructions the translated microcode ran in the level (4,600 a call, 14,000
calls in 15 s). It is the third of the particle programs, beside 0x000 (upright quads) and 0x110
(turned quads): each particle a quad stretched along its motion, which is how the level's rain
is drawn.

`Streaks` (port/render/vu1_handlers.cpp) is its handler. `SLY1_VU1_CHECK=1` over 600 frames of
the level: 1,438,611 handler runs compared with the interpreter's, 9,360 of them this one,
0 differed. On the host the level's VU1 time went from 2.35 s to 2.02 s in 15 s of play (the
handler takes 0.21 s for what the translated code took about 0.5 s for).

## FINISH told to the game from the render thread (2026-11-19)

`SLY1_TIMELINE` on the Switch in A Stealthy Approach, one frame from its vertical blank: the
game's render thread swaps the buffers and sends an upload and the frame's list of VIF1's
(2 ms); the port's render thread does the list; FINISH is raised when the GS thread has drawn
it (0.5 ms after); the game sends two more packets, each waited for the same way (2.3 ms); then
it waits for the next blank. A frame cost those one after the other. With the particle streaks
in native code the three threads took 13, 14 and 12 ms a frame there, and the frame still two
blanks.

The game is now told a packet is finished when the port's render thread has done with it. The
render thread marks an item for the GS thread whose last tag is the game's FINISH (the tags are
walked from the first: a tag of one A+D register, the write of 0x61, the item's last quadword),
and raises FINISH at the end of its own item, after the parts run beside it are in; the GS
thread, coming to that write, only counts it off. A FINISH not found that way is raised by the
GS thread as before. The GS thread may then run a frame behind, and no further: the render
thread does not begin a list while more than three FINISHes (a frame's) are owed. What is shown
comes a frame later. `SLY1_FINISH_LAG=0`: as before. `SLY1_PERF_LOG=1` says how many were told
from each thread (`perffinish:`).

Checked on the host with the GS thread: 1,299 frames of the level under lockstep hash the same
with it and without; in real time every FINISH (three a frame) is told from the render thread
and none owed at the end; the opening movie plays to its end with no error in the stream. Not
yet measured on the Switch.

On the Switch (9269bda) it made the game unusable, and it was off unless `SLY1_FINISH_LAG=1` (on since, see below): the
controls answered about a second late. `perffinish:` showed why: the GS thread owed 108 FINISHes
(36 frames) and took 27.9 ms a frame where it had taken 14. Its buffer swap waits for the
display, so by itself it shows little more than 30 frames a second; told to go on, the game made
frames faster than that, and the wait that was to keep the GS thread within a frame gives up
after a tenth of a second. Before this is tried again the swap must not hold the GS thread (the
next frame drawn while the last waits for the display), and the bound must hold without a time
limit. The host showed none of it: its GS thread does not wait for a display.

### The lag bounded without a time limit (2026-11-20)

Two faults of the first try are put right, still behind `SLY1_FINISH_LAG=1`. The wait before a
list of VIF1's was made only for lists copied for the render thread, not for those read in place
(the large ones, which a frame's list nearly always is): the bound was hardly ever applied. And it
gave up after a tenth of a second. It is now made for both and has no limit of time (half a second
of it is said). `SLY1_PERF_LOG=1` prints `perfheld:`, how long a frame is held from its list
reaching the render thread to the swap that gives it to the display, and how long the render
thread waited for the GS thread; `perfgpu:` (Switch), how long a present waited for a screen image
and for the GPU. `SLY1_SLOW_GS=ms` makes each swap that much longer, for trying the bound on a
fast machine: with 25 ms the GS thread owes four FINISHes at most and a frame is shown each
28.8 ms (34.2 without the lag). `SLY1_VSYNC=1` (Switch) takes the game's vertical blank from the
display's own event in place of the program's clock.


### A light volume's cone marks the frame's alpha

A searchlight is drawn in three passes (the game's BuildAnimRecordGifPackets, kind 4; VU1's
program 0x6DA): its cone, textured and added to the frame under the alpha test NEVER with
FB_ONLY; the cone's back faces under blend 0x6A, which write alpha 0x80 and no colour; and a
tint (blend 0x48) under the destination alpha test with DATM 0, in the record's colour halved.
The cone's own alpha, under 0x80, is the mark the tint is tested against, and the second pass
takes the mark away where the scene is behind the whole cone: the tint lands where the scene is
inside it. Only the tint changes with the alarm (128 110 0 to 128 0 0); the cone's texture and
its vertices' colours stay as they were.

The cone's pass went into the effects' half-size picture, which writes nothing to the frame's
alpha; and drawn into the frame with the blend state's factors the GPU gave the frame's alpha the
colour's factors (over 0x80 across the cone). Either way no mark was left, the tint drew nowhere,
and a searchlight lit nothing, in yellow or in red. Such a run (blend 0x48, the alpha test NEVER,
FB_ONLY, no DATE) is now drawn into the frame the way a run into a small target is, the blend's
coefficients in the pixel shader, so that the alpha written is the source's.

`SLY1_GPU_TINT_LOG=1` prints the tints' colours, `2` every blended run whose first colour is not
a grey and each cone's texture; `SLY1_GPU_BLEND_LOG=N` prints frame N's blended draws in order.


## A surface drawn with its lights' shares (2026-11-20)

The game draws a world surface three times: the surface itself (blend 0x44, the alpha test
against 126 with FB_ONLY, depth GREATER) and then, with the same positions, two batches that each
add one light's share to it (blend 0x48, the alpha test NEVER with RGB_ONLY, depth GEQUAL), each
with its own colours and its own palette of the same texture (TEX0 differs in CBP only), and fog
towards black. A share whose colours are all black adds nothing and is left out
(`SLY1_GPU_NO_BLACK_SKIP=1` draws it).

The surface's batch is held back (Renderer::Layer) and the shares that follow it join it: one
draw whose vertices carry the three colours (batch_vsh.glsl's two more streams) and whose pixel
shader adds the shares to the surface's blended colour. Anything else that goes to the GPU draws
what is held first (FlushHeld), so the game's order stays.

- **A surface with a triangle twice** (a leaf's two faces, the same three corners: Doubled) gets
  its light twice from the game, once for each triangle of the pair under GEQUAL, where the
  surface itself is drawn once under GREATER. It is drawn by itself and its shares after it, in
  one draw of their own under their own depth test: two draws for three.
- **The shader matters more than the count of draws.** gs_fsh.glsl has discards, so the GPU runs
  it for the pixels the depth test then drops. Drawn with it, the joined surface cost the
  Switch's GPU more than the three draws (22.5 ms a frame at A Stealthy Approach's start against
  15.7); with gs_layers_fsh.glsl, which like gs_lite_fsh.glsl has none, 13.0 ms. The emulator
  showed none of this (1.9 ms either way against 3.2): GPU time is measured on the Switch.
- The picture is the three draws' within rounding: in a lockstep capture 0.06% of the colour
  values differ by more than 8.

`SLY1_GPU_NO_LAYERS=1` draws as the game does; `SLY1_GPU_SHARES_AFTER=1` draws every surface's
shares after it. `SLY1_GPU_DRAIN=1` waits for the GPU before each present and `perfgpu:` says
how long (what a frame costs the GPU, since a frame's commands are sent at its end);
`SLY1_GPU_VARY=1|2|3` changes what is drawn every 120 frames and prints the GPU's time for each
kind (`vary:`): 1 leaves kinds of batches out, 2 kinds of runs, 3 compares this section's draw
(kind 0), the game's three (odd kinds), the joined draw with gs_fsh.glsl (2, 6) and the joined
draw for surfaces without a doubled triangle only (4).


## Sixty frames a second at A Stealthy Approach's start (2026-11-20)

With the surfaces drawn with their shares (above) the level's first view was shown every
17.5 ms on the Switch, not 16.7: one frame in eighteen took two blanks, exactly. `SLY1_TIMELINE`
(now with `SLY1_TIMELINE_EVENTS=N` for more than 400 events, the GS thread's present and each
read of the disc) showed the game's main thread busy 14 ms in that frame, between its post of
the frame-ready semaphore and its wait for the frame-done one, where it otherwise takes a
fiftieth of a millisecond; FINISH, raised on time, was not served until it let go. The host
shows the same rhythm (0.35 ms). `SLY1_SAMPLE=1` now samples the main thread too in that window
once it has been open 2 ms, with what its stack holds of code addresses (`samplestack:`): it was
waiting, switched to the thread the game's memory card code runs its requests on. The game asks
the card's free space three times a second, and the port walked the save folder for it each
time, about 12 ms on the SD card, with the game's other threads held (they run in turn). The
used space is now kept until the card is written to (port/platform/memory_card.cpp).

After it: 16.7 ms a frame over 90 s, a frame held 16.0 ms from its list to the display's queue
(16.5 at most); render thread 9.6 ms, GS thread 10.9, the game's threads 8.8. The lag of FINISH
is on unless `SLY1_FINISH_LAG=0`.

The display's own vertical blank is still not had: a second opening of the display is refused
(0x1272) and its event by the display's number too (0xE72). The game's blank stays this
program's clock.

### Walking: the render thread's allocations (2026-11-20)

Played through A Stealthy Approach the Switch held 16.7 ms only standing still; walking, most
frames took two blanks with the render thread at 16 to 18 ms and no wait for the GPU. (The game
queues its list 2 ms after the blank and cannot end its frame before the render thread has, so
the render thread has about 13 ms.) `SLY1_SAMPLE=2` samples the render and GS threads only
while fewer than 48 frames a second are shown and prints each sample's callers by the frame
records (`samplechain:`; nothing is allocated while a thread is stopped, which may be inside the
allocator: that stopped the program). `SLY1_PERF_LOG=1` has a line for the parts' jobs
(`perfjobs:`).

- A third of the render thread's work there was the allocator (operator new 23%, free 9%, its
  lock contended with the GS thread's frees 7%): the buffers of the items it queues for the GS
  thread. The stock of emptied buffers held 256 and the GS thread gave back 256 of each lot it
  took, where a frame has a thousand items: the rest were freed and allocated anew each frame.
  Both are 2,048 now, and no buffer is allocated in a still scene (835 a frame from the stock).
- A part is kept anew only when two lists running have it the same (`SLY1_PART_KEEP_TWICE=0`:
  at once). Parts whose header changes with the camera were kept each time they were met, 4 to
  5 a frame. Worth little by itself: such a part is run in full either way.
- Not the cause: the render thread's wait for jobs at its list's end (0.1 ms a frame;
  `SLY1_JOB_WAIT_US` sets after how long a job begun elsewhere is run again).

### A tour for timing without a player (2026-11-20)

`SLY1_TOUR=frames:x,y,z;x,y,z;...` (with `--level N`, and without `SLY1_LEVEL_FRAMES`) places
the player at each place in turn for that many frames; `SLY1_PAD_TURN=N` turns the camera from
the Nth pad read on, the player standing; `tools/tour_report.py LOG` gives each stop's frame time
and the threads' times from `SLY1_PERF_LOG=1`. The places are a played route's (`perfpos:`).
`SLY1_PAD_WALK=N` holds the stick forward too, but a player walking blind dies within a stop or
two, and a dead player's frames are no measure (50 ms a frame, not understood).

A Stealthy Approach, six stops of 600 frames where the player survives
(`600:2278,-17153,732;1436,-11947,1167;-458,-10290,600;-2802,-11291,786;-5223,-12472,729;-15741,-7938,-189`),
on the Switch at 3e85119 plus the tour: 20.2, 17.4, 16.7, 17.3, 16.9 and 20.8 ms a frame, 18.2
the mean. It is lighter than play: walked by hand the same level's slow stretches are 24 to 27 ms
with the render thread at 15 to 17 ms and the GS thread at 16 to 19. The tour compares builds; it
does not say what a player gets.

### The jobs off the frame's path (2026-11-20)

`SLY1_TIMELINE` over 229 frames walked on the Switch (211 of one blank, 18 of two), from the
game's swap: the list queued at 1.7 ms, walked by 7.4 (8.2 in the frames of two blanks), the
parts' jobs in by 10.7 (15.8), FINISH told at 11.0 (16.1), the game's frame done at 12.5 (17.4).
The render thread's wait for the jobs at its list's end, 3 ms standing and 7 to 8 walking, was
what took a frame past its blank: no thread's own work did. (The averages of `SLY1_PERF_LOG`
and the samples had not shown it: three cuts made from them changed nothing a player could
measure.)

The wait was there because a job reads its part's packet where it lies, in memory the game
writes again once it is told the list is finished. A job now has a copy of its packet, made
when the job is (`JobCopy`: WalkPart into a buffer of 48 K words that the job keeps; a longer
packet is read where it lies and the list waits as before). With every job of a list on a copy
the game is told at once, and the render thread sees the jobs done when it has nothing queued,
and before its next list in any case. `SLY1_JOB_COPY=0`: as before. In Ryujinx at A Stealthy
Approach's start: every job on a copy, 102,000 words copied a frame, and a lockstep capture of
frame 1290 the same picture to the last value.

## A part's numbers left to the renderer (2026-11-21)

The first step of the native draw path (docs/DESIGN.md, "The port's own functions"): what the
game's programs do to every vertex of a part by one number of the part's own is done by the
vertex shader, so the part's vertices stay the same from frame to frame and stay kept.

What made a part drawn from its cache a job, walking through A Stealthy Approach
(`SLY1_PART_DIFF=1` on the host's tour, by lighting mode): of some 150 such parts a frame, 14
had two words of their packet other each frame, 6 the header's 9.x, and some 7 came in the
other half of VIF1's double buffer than they were kept in. With the lockouts that follow a part
found changed (`fickle`), that was most of the 45 jobs a frame the Switch ran for cached parts
in play.

- **The alpha's scale** (the header's 9.x: a part that fades with its distance). The program at
  0x4A9 multiplies each vertex's alpha by it and truncates. For a batch whose transform is the
  renderer's, `Scale4A9` now leaves the alpha as it is and the batch carries the scale
  (`gs::Batch::alphaScale`); batch_vsh.glsl does `floor(alpha * scale)` for the colour and for
  the two lights' shares. A batch that goes back to the program (`Materialise`) has the scale
  put in its colours then. A kept part whose batches all carry the scale is drawn again with
  any 9.x (`PartHeaderSame`'s `alphaFree`); one with a batch that does not is compared as before
  (`PartRecord::alphaStrict`).
- **A texture that slides** (22.x and y, which the program at 0x4F8 adds to every vertex's S
  and T; the game writes them anew each frame in the cached packet, one unpack of two words:
  `kStUnpack`). The two words are left out of the part's hash, where they lie is noted with
  the record (`stAt`, as a TEX0 write's place is), and each kept batch the program ran on
  (`stApplied`) knows which unpack was its own (`stSource`: the last VIF1 had met when the batch
  was queued). Drawn again, the batch is given how far the offset now is from the one in its
  vertices (`gs::Batch::stOffset`), which the shader adds. Not exact: (S + a) + (b - a) is S + b
  to the last bit or two. A part with such an unpack no kept batch answers for is compared by
  the words' values (`stStrict`).
- **A part of its own for each half of the double buffer** (`SLY1_PART_HALVES=0`: one). The game
  turns VIF1's two buffers about from mesh to mesh, so a part comes in either half by what was
  drawn before it, with another packet (a cached part has two) and other registers. Under one
  key it was kept anew whenever something drawn before it came into view or left it: 4,800
  parts found changed by their hash over the tour's 1,200 frames, 90 with a key for each half.
- **Registers a part does not read.** A part drawn the long way begins with BASE and OFFSET, so
  those, TOPS and the double buffer's flag as it is met are nothing to it (the flag goes by how
  many programs the part before ran). And a packet with no masked unpack before an STMASK of its
  own (`PartHasher::maskRead`) is the same part whatever mask it is met with; drawn again, it
  leaves the mask as it found it unless it sets one. 2,000 parts found changed by their
  registers over the tour, then none.
- **A part marked as moving** (its vertices other where the hash does not look) was run as a
  job for as long as its sampled hash stayed the same: for good, when what changed was its
  cache's colours written once more. The mark now ends after 120 frames, twice as long each
  time (to 3,840), and the part is kept anew.

`SLY1_PART_PARAMS=0` turns the first two off. `partparams:` (with `partcounts:`) counts the parts
drawn again with another scale or offset; `SLY1_PART_REGS_LOG=1` says which registers were other
when a kept part was not drawn again (`partdiff:`).

On the host's tour of A Stealthy Approach (three stops of 400 frames, the camera turning, the
counting renderer taking batches): 162,000 parts drawn again against 122,000, 2,700 kept anew
against 3,300, and by kind a frame (drawn again / job / in full):

| | before | after |
| --- | --- | --- |
| from its cache, unlit | 2.9 / 3.9 / 0.3 | 5.0 / 1.9 / 0.3 |
| from its cache, lit by it | 54.7 / 10.6 / 2.3 | 64.5 / 1.5 / 2.1 |
| the long way, unlit | 0.4 / 5.7 / 4.1 | 6.6 / 3.6 / 0.3 |
| the long way, lit by its cache | 1.2 / 2.6 / 0.8 | 1.5 / 3.1 / 0.1 |
| the long way, lit by VU1 (mode 4) | 1.1 / 8.0 / 3.8 | 2.4 / 10.1 / 0.5 |

What is left is the next steps' work: the parts VU1 lights or skins afresh (mode 4), and the
parts drawn the long way whose mesh data has six quadwords at 22 other each frame.

Checked on the host: level 4 under lockstep with the GS thread and the software batches
(`SLY1_GS_SOFT_BATCH=1`, `SLY1_LOAD_WAIT=900` for the load under the software rasteriser), the
camera turning, every 60th frame to 2,340 against the same run with `SLY1_PART_REPLAY=0`: the
25 frames in the level differ by 1 of a colour's 255 at most (the slid textures). `--verify-levels`
with the counting renderer taking batches, 240 frames a level: all 46 levels, 254,000 parts drawn
again with another scale and 48,000 with an offset moved. Not yet run on the Switch.

### On the Switch, and what it showed next (2026-11-21)

Played at ca9d740 (A Stealthy Approach and on, `SLY1_PERF_LOG=1`), against the play at a8a20da the
same day, in the windows over 18.5 ms a frame: parts from their cache run again 39 a frame, then
10; all parts run again 68, then 39; such windows 24% of those in 3D, then 11% (two plays, not
one route). Left there: the parts VU1 lights afresh (mode 4), 20 jobs a frame; the GS thread at
14 to 16 ms; and in the worst windows 9 parts a frame being kept and 10 watched, 5 to 8 ms of
the render thread.

Two things found on the host after it:

- **Everything kept was let go at once** when it passed 48 MB, and a whole scene's parts were
  then kept anew over the next frames. With a record for each half of the double buffer the
  tour reached the 48 MB within a minute. Now the parts not drawn for longest are let go until
  an eighth of the limit is free (`PartRecord::usedFrame`; `SLY1_PART_KEEP_MB`): 507 parts let go
  over the tour, none of them drawn in the 120 frames before; at 24 MB a third of them had been.
- **A job's registers** are matched as a kept part's are (the mask when the packet reads none,
  and for a part drawn the long way not what its BASE and OFFSET set anew).

`perfkeep:` says a frame why parts are kept anew and watched, what is kept and how often parts
were let go for room; `perfkept:` (the GPU renderer) the kept batches' memory; and the program
says at its start how much memory it may use.

### The frames that come late (2026-11-22)

Played at d502426 the Switch's windows over 18.5 ms were 8% of those in 3D, with 23% between
17.2 and 18.5: single frames of two blanks among frames of one, which a second's averages do
not explain (`SLY1_SAMPLE=1` there: the render thread idle 57% of the time and the GS thread
27%, and no function of the GS thread's over 10%). `SLY1_PERF_LOG` now prints a line for each
frame shown more than 20 ms after the one before (`late:`, 8 a second at most): what each thread
did between the two, the parts kept, new or watched, the uploads, the batches.

157 such frames in 3D over one play: on average 10 parts kept and 11 new or watched in the frame
(about 1 of each in an ordinary frame), 6 ms of parts run in full on the render thread, 22
uploads, 710 batches against 500. They are the frames in which the view turns onto a new place.
Two things done for it:

- **A part met for the first time is a job at once** when another part's packet of the same
  shape has shown the registers it leaves (`g_shapeRegs`; `SLY1_PART_SHAPE_REGS=0`: not). It was
  run on the render thread and watched before. On the host's tour: 2,005 parts made jobs so, and
  the parts watched 0.37 a frame against 1.35.
- **No more keeping in one list than 2 ms allow** (`SLY1_PART_KEEP_US`); the rest wait as jobs.
  (Three parts a list fell behind: a sixth fewer parts drawn again over the tour.)

Also: `SLY1_PART_JOBS_CHECK=1` ran its own pass before the job's packet was copied and read the
copy left from the job's last use (it hung in the first level); it now runs after the copy.
With it, `--verify-levels` at 240 frames a level: 549,356 jobs over the 46 levels, 0 wrong.
Level 4 under lockstep with the software batches: 28 frames within 1 of 255 of
`SLY1_PART_REPLAY=0`.

### A part kept from its job (2026-11-22)

Played at abf3c72 the limit on keeping did harm: at 2 ms a list the Switch put off 50,759
keepings in one play, the parts waiting were jobs (92 in a late frame against 47), and 2.1% of
the frames in 3D came late against 1.1% (the camera was turned fast on purpose in that play, so
the two are not one route). The other change held: 2.7 parts new or watched in a late frame
against 10.6. The limit is off now (`SLY1_PART_KEEP_US` sets one), and keeping no longer needs
the render thread:

- A job collects exactly what a recording collects (the packets and batches the part queues for
  the GS). A part that is to be kept and can be a job is made one, with the matrix its batches
  must have, and when the list's jobs are all done (`PartJobsKeep`, at the next list's start)
  what the job gave is copied to the record, each batch given its key, and judged by the code
  that judges the render thread's own recording (`PartKeepFinish`). `SLY1_PART_KEEP_JOB=0`: kept
  by the render thread's run as before.
- A job's thread says which of the packet's texture offsets a batch has (`t_jobStCount`), and
  whether a batch of the run was not handed over as unpacked (`Collect::trouble`).
- A part met for the first time gets its record at once, so that it can be a job by its shape.

On the host's tour: 2,308 parts kept, all from their jobs (199 refused, as before); no part run
on the render thread to be kept (1.25 a frame before) or because it was not known (0.74); the
parts drawn again unchanged. Checked: level 4 under lockstep, 33 frames within 1 of 255 of
`SLY1_PART_REPLAY=0`; `--verify-levels` with `SLY1_PART_JOBS_CHECK=1`, 240 frames a level: all
46 levels, 562,679 jobs, 0 wrong, 29,836 parts kept from their jobs.

A start on the Switch faulted once more before any level (the game's GIF channel pointer,
0x00261F1C, holding 0x4F265060 when InitDisplay sent its first packet, after InitDmaChannels
had set it): the message for a chain on an unknown channel now prints the words around the
five channel pointers.

## A lit part's colours left to the renderer (2026-11-22)

What still made jobs after the parts' numbers and the keeping were dealt with, by the host's
`SLY1_PART_DIFF` census (Paris is level 2): parts lit by VU1 (mode 4) whose light moves (three
words of the light's packet at 26, its place, other each frame: 17 parts a frame in Paris) or
that turn (the header's inverse matrix and centre); parts with a texture laid on by a matrix
that changes each frame (six quadwords at 22: 9 a frame); and parts whose programs make vertices
for the frame's view (particles, sprites: 19 a frame, refused when kept). The first are the
most, in both levels played, and they change every frame: only lighting them in the vertex
shader makes them parts that are kept.

How the programs light a batch in mode 4 (`SLY1_VU1_SEQ=1`): 0x051 (setup), a light's program
for each light (0x17E directional, 0x1A6 point: both end in 0x0FE, four vertices at a time, the
point light taken as a direction from the object's centre with the attenuation there), 0x242
(the colours made final), then the transform and the kicks: the surface with each vertex's
quadword 1, its two shares of light with quadword 2 and the colours' array.

**The first stage** (this one) is the arithmetic and its inputs. `gs::LitColoursOf`
(port/gs_backend.h) makes a vertex's three colours from its unit normal, its base colour and
`gs::LitParams`: for each light a direction, base, scale and colour, the two sums' start, vf01
and the colours' scale. The handlers note those as they run (`LitCapture`, by TOP, in the thread
that runs the part: the lights in `LightFourWide`, the normals and base colours in `LitColours`;
any other light, or a program left to the microcode, drops the notes), check them against the
sums the programs left before the colours are made final, and give them to one batch only.
`SLY1_LIT_CHECK=1` compares the formula's colours with the programs' at each kick: over the
host's tours of Paris and A Stealthy Approach every value was the same (47 million in Paris).

**The second stage: a lit part drawn again with this frame's lighting.**

- A lit batch carries what was noted of its lighting after its vertices (`gs::Batch::lit`: the
  parameters, what its mesh adds to the second sum's start, each vertex's normal and base
  colour). `SLY1_LIT_GPU=0`: not.
- A part in mode 4 has its lights' words left out of its hash (`LightUnpack`: the unpacks of a
  light's packet, RefreshConeVolumeShape's: the colour to 22, the scale and base to 23 and 24, a
  direction to 25 or an attenuation to 25 and a place to 26), and where they lie is noted
  (`PartRecord::litAt`, grouped into the part's lights: every mesh refers to the same packets).
  Its header is compared by the fog and the mode and count alone (`PartHeaderSame`'s `litFree`).
- Drawn again, the part is given the lighting's parameters made for this frame (`PartLitNow`: a
  point light's direction from the centre and its fade there, each direction turned by the
  header's inverse matrix, as 0x142 and 0x0FE make them), and each kept batch its own with its
  start and vf01 (`gs::Batch::litNow`). The renderer makes the kick's colours from them
  (`gs::LitColoursNow`): the Switch's in `Renderer::DrawBatch`, on the GS thread, into the
  frame's memory in place of the kept colours; the software batches and the counting renderer
  likewise.
- A part with a lit batch that has no lighting noted, or with lights not of the two kinds, is
  compared by the words and the whole header as before (`litStrict`). `SLY1_LIT_REPLAY=0`: all.

`SLY1_LIT_NOW_CHECK=1` (with `SLY1_PART_JOBS=0`) runs such a part all the same and compares what
the programs light it with against what it would have been given: on the host's tour of Paris
79,892 batches the same and 108 not, those lit by another light than the part was kept with
(a part's hash is the helper's, of the list before: for one frame after its lights change it is
lit by the old ones, as it is drawn as it was for one frame after anything else of it changes).

On the host's tour of Paris: the parts in mode 4 drawn again 23.9 a frame against 7.1, their
jobs 8.6 against 25.4 (what is left are skinned); all jobs 46.6 against 63.0. Checked: Paris
under lockstep with the software batches, 28 frames within 3 of 255 of `SLY1_PART_REPLAY=0`;
`--verify-levels` with `SLY1_PART_JOBS_CHECK=1`, 240 frames a level: all 46 levels, 282,013
parts lit by their frame's lights, 387,167 jobs against 562,679, 2 of them leaving other
registers than foretold (none before; not looked into yet).

The colours are made on the GS thread, a vertex at a time, not in the shader: the renderer's
vertex layout and its three colour streams stay as they are. The shader is the next step if
the GS thread's time for it shows.

## The GS thread by kind of item, and the outlines' edges (2026-11-22)

With the parts' jobs down to some 25 a frame the Switch's GS thread is what limits a frame: 12.4
ms a frame over a play of Into the Machine and Prowling the Grounds, 17.3 in the windows over
18.5 ms. `perfgs:` (and `lategs:` after a late frame's line) says where, by the path of the
items it takes. In the windows at 60 frames a second, a frame: parts drawn again 5.84 ms (168
parts: 35 microseconds each), VU1's packets 2.74 ms (1,109), batches 1.62 ms (295), the GIF
channel's 0.83 ms (311 KB of image data a frame), calls 0.66 ms; 1,420 palette loads a frame,
85 of them read from the GS's memory.

`SLY1_PATH1_CENSUS=1` (the host) says what VU1's packets are: over half are the outlines' (0x26D
kicks one for each edge it draws, 620 a frame in A Stealthy Approach, each a tag with a
register write and a line and the same again for the line's depth alone).

- **VU1's packets join in one item** (`Path1Last`; `SLY1_PATH1_JOIN=0`: an item each): 1,092
  items a frame became 316 on the host's tour.
- **A run of outline edges is one tag of lines** (`OutlineJoin`; `SLY1_OUTLINE_JOIN=0`: not): as
  the packets join, edges with the same tags and writes become the first edge's first half, one
  tag with all the other edges' vertices, and the last edge's second half. The same lines are
  drawn by the first write's state, and the registers and the vertex queue are left as the last
  edge left them; the lines for the depth alone, which the GPU renderer leaves out, are not sent
  but for the last.

On the host's tour the GS thread's time for VU1's packets went from 0.58 to 0.41 ms a frame
(the host is some ten times the Switch). Checked: level 4 under lockstep with the software
batches, 28 frames within 1 of 255 of `SLY1_PART_REPLAY=0` (the software drawing draws each
edge's second line where it has one: the pictures are the same without them); `--verify-levels`
with `SLY1_PART_JOBS_CHECK=1`: all 46 levels, 388,710 jobs, 0 wrong, 7.4 million edges put with
the edge before.

### On the Switch, and the parts drawn again (2026-11-22)

Played at a1497e8 (Paris), the GS thread's time for VU1's packets was 1.26 ms a frame for 365
items in the windows at 60 frames a second, against 2.74 ms for 1,109 in the play before; the
outlines looked as before. What is left is the parts drawn again: 7.85 ms a frame there for 271
parts (29 microseconds each), 12.35 ms for 424 in the windows over 18.5 ms.

Three things for them, none of which changes what is drawn:

- **A kept packet is its register writes** (`PartKeepFinish`: path 7, a register and its value's
  halves, a tag's PRIM first; `SLY1_PART_WRITES=0`: the packet as it was). The GS thread makes
  the writes without the GIF's reading of tags and quadwords. A TEX0 moved is put at its place
  among the writes.
- **A texture found before, with nothing uploaded since, is taken without its pages being
  walked** (`Renderer::TextureFor`: the walk was for the latest upload to them, which cannot be
  another).
- **Whether a kept batch has a triangle twice** is kept with the batch (`Kept::doubled`), not
  looked up by its key in a table of its own.

On the host's tour the GS thread's time for the parts drawn again went from 0.95 to 0.75 ms a
frame with the first (the other two are the Switch renderer's). Checked: level 4 under
lockstep, 28 frames within 1 of 255 of `SLY1_PART_REPLAY=0`; `--verify-levels` with
`SLY1_PART_JOBS_CHECK=1`: all 46 levels, 64,174 parts drawn again with a texture moved, 389,003
jobs, 1 wrong (0 to 2 in the runs since a part's first sight makes it a job by its shape: a job
that leaves other registers than foretold is caught and its part run in its turn after; the
cause is not found).

## The survey of all levels, spot lights, and skinned parts drawn again (2026-11-22)

**The survey.** `SLY1_TOUR=arrivals` with `--verify-levels` toured every level's arrival points
with the camera turning (39 of the 46 have them; Treasure in the Depths has no player object).
A second pass with `SLY1_PART_DIFF` faulted at the eleventh level: the census's table was counted
by the render thread and printed and cleared by the host's main thread at each level's end. It
is now taken under a lock (`DiffCount`), and the table of packets seen is cleared by the render
thread itself. What changed in parts' packets over the 46 levels, the largest: the bones of
skinned parts (some 90,000 changes for each bone's place), six quadwords to 22 in parts drawn
the long way (262,000: the inputs of 0x076, below, and of the sway), eight quadwords to 48 in
unlit cached parts (126,000, not looked into), texture offsets in parts drawn the long way
(53,000).

**Six quadwords to 22.** Two programs read them. 0x750, 0x752 and 0x754 of chunk 51A89CD8 (run
on the interpreter: no handler) are a sway: rows 22 to 25 turned into the part's space by the
header's 4 to 7 (0x22B), and each vertex's position moved by its four weights (the array at
11.w) times those rows. The kept vertices are now moved by how much the rows changed
(`gs::Batch::sway`, `gs::SwayQuads`, `kSwayUnpack`; `SLY1_SWAY=0`: not), but the sway runs a few
hundred times in a whole tour: most of those quadwords are 0x076's (sprites drawn at a mesh's
transformed vertices through another matrix), whose parts are refused when kept (their packets
carry vertices made for the frame's view). Not done.

**Texture offsets in parts drawn the long way** are left out of the hash too where they lie in
a REF of a mesh's data (`PartHasher::stable`); those in the part's own chain stay in the hash.

**Spot lights.** A part lit four vertices at a time by a spot light (0x1DE with 9.y 4: 0x154,
then 0x0FE) is now drawn again with this frame's lighting: the light's view and projection (28
to 31) take the object's centre; outside the view the program goes on as a directional light
from 25 with a scale of 23, inside the cone's factor there (the two ramps at 25 and 26) scales
the light (`PartLitNow`). `SLY1_LIT_CHECK=1` over Treasure in the Depths: 21.9 million values, 2
other by one.

**Skinned parts** (0x401 and 0x411 of chunk 2DDC9AA2: each vertex's position through four
matrices blended by its weights; its normal through their 3x3 parts, made unit length):

- What the skinning begins from is noted (`SkinCapture`): the positions at 0x401, the normals at
  0x411 (they are unpacked after the positions are skinned: read at 0x401 they were another
  batch's), the weights, the four matrices. A morph or a program on the microcode after it drops
  it. The batch carries it (`gs::Batch::skin`).
- The bones (`kBoneUnpack`: four quadwords to a mesh's bone place) are left out of the hash. They
  are in the part's own chain, written each frame at another place: noted as words from the
  chain's start (`MaskAt`), and taken only where the same unpack's code is before them (the
  verdict may be the helper's, of the list before, and a chain laid out otherwise has other
  words there: 443 parts over the 46 levels).
- Which bone each of a batch's four matrices was is the last bone unpack to its place
  (`Vif::LastBoneAt`, `BoneSources`); a place no vertex weighs needs none.
- Drawn again, each batch's positions and normals are skinned by this frame's bones on the GS
  thread (`gs::SkinQuads`), and its colours made from the new normals and this frame's lights.
  The Switch's renderer puts the positions in the frame's memory (`Source::vertexFrame`).

Checks: `SLY1_SKIN_CHECK=2` (the formula against 0x401's own output: 2,048 batches the same),
`=3` (the bone taken against the matrix the loop used: 4,096 the same), `=5` (the bones by their
places against a walk of the chain), and `SLY1_SKIN_VERIFY=1` with `SLY1_PART_JOBS=0` (a part that
would be drawn again run as ever, each batch compared with what drawing it again would give):
16,384 batches with the same positions, 16,384 lit with 68 colours further than one (the frame
after a part's lights change). Level 4 under lockstep with the software batches: 39 frames
within 1 of 255 of `SLY1_PART_REPLAY=0` (with the normals read too early, Sly's colours were up
to 150 off from frame 1,140 on); Paris: 28 frames within 3. On the host's tour of A Stealthy
Approach the parts lit by VU1 run as jobs 3.7 a frame against 6.6, 5,474 skinned parts drawn
again; `--verify-levels` with `SLY1_PART_JOBS_CHECK=1`: all 46 levels, 32,132 skinned parts drawn
again, 333,185 jobs against 389,003, 4 leaving other registers than foretold (caught, as before).
`SLY1_SKIN=0` turns the skinning off.

### On the Switch: what the skinning costs (2026-11-22)

At 6d6b145 with `SLY1_SKIN=ab` (skinning anew on and off in turn, 600 frames each), one play of
A Stealthy Approach, in the windows in 3D away from the switches: on, the helper's jobs 3.8 ms a
frame and the GS thread 32 microseconds for each part drawn again; off, 9.1 ms and 26. The frame
is the GS thread's: windows at 60 frames a second 71% on, 83% off, the mean frame 17.1 ms both.
So the skinning is off unless `SLY1_SKIN=1`, until it is done off the GS thread (the helper has
the time it frees: the positions and colours could be made there when the part is decided).
Over the whole play: 77% of windows in 3D at 60, 5% slow, 1.2% of frames late; the GS thread
11.7 ms a frame, its parts drawn again 5.7 ms for 197 parts.

### The per-frame work of parts drawn again, made by the helper (2026-11-22)

What a kept batch is drawn with each frame (its vertices skinned and swayed, its colours lit anew)
is one function now (`BatchNow`, with `ReplayLayout` reading the item's words). For a part with any
of it, PartDecide makes a task (`PrepTask`, of a pool) that the helper takes between jobs: it runs
`BatchNow` for each of the part's batches and keeps the moved vertices and each kick's colours
(three kicks' made at once, `gs::LitColoursAll`). The GS thread takes what it made
(`gs::Batch::coloursNow`); it makes the task itself if the helper has not begun it, and if the
helper is at it waits up to 0.2 ms, then does the work its own way. `SLY1_PART_PREP=0`: the GS
thread does it all, as before. This is for the skinning above, which cost the GS thread more than
it saved, but the lit parts' colours move off the GS thread too.

On the host's tour of A Stealthy Approach with `SLY1_SKIN=1`: 30,176 tasks made by the helper,
14,529 by the GS thread, 49 done its own way, 0.14 s of waiting over some 2,300 frames. Level 4
under lockstep: 29 frames within 1 of 255 of `SLY1_PART_REPLAY=0`. Not measured on the Switch.

### The GS thread's time for a part drawn again (2026-11-22)

At 6d6b145 on the Switch a part drawn again cost the GS thread 26 microseconds (32 with the
skinning); the host cannot say where (its renderer only counts what it is given). Two things:

- `perfgs:` now splits it: the batches (drawn, with their per-frame work), the packets kept as
  register writes, the packets through the GIF.
- A kept part's items are in one piece of memory, one after another at 64 bytes, as the GS thread
  goes through them (`PartKeepFinish`; `SLY1_PART_PACK=0`: each in a block of its own, as before):
  the Switch's cores wait on memory, and the items were scattered over the heap.

Level 4 under lockstep: 29 frames within 1 of 255 of `SLY1_PART_REPLAY=0`.

### The sprites' pass as a batch (2026-11-22)

0x076 of chunk E1624411 runs on a mesh already transformed: it takes each vertex's screen position
back to its clip position, through a matrix (22 to 25 times data memory's 8 to 11, made in place
by 0x22B) to S, T and Q, keeps Q within 27's x and y (moving S and T by half the change, the alpha
0 below 27's w when it did), colours every vertex with 26, and kicks the mesh's own tag again: a
texture laid on the mesh from another view (the shadows under characters, the light in Treasure in
the Depths). It read the screen positions, so every mesh it ran on was transformed here, and its
part was refused when kept (its packet carried vertices).

When the mesh's transform is the renderer's, 0x076 is now a second batch of the same record
(`gs::Batch::projected`: `proj`, `projLimits`, `projColour`; `gs::ProjectedOf` is the arithmetic,
batch_vsh.glsl does it on the Switch, the software batches and the counting renderer on the host),
after the packet at 28 (`SpritesAsBatch`; `SLY1_GPU_SPRITES=0`: as before). A part drawn again
has its batch laid on with this frame's matrix, colour and limits, made of the unpack to 22 it came
from (`projSource`, counted as a sway's) and data memory's 8 to 11 now.

On the host's tours: Treasure in the Depths, jobs 9.0 a frame against 83.5 (the parts drawn the
long way and lit by their cache: 0.5 against 75); A Cunning Disguise, 42.6 against 69.6. Checked:
level 4 under lockstep, 29 frames within 1 of 255 of `SLY1_PART_REPLAY=0` (with and without parts
drawn again); level 10, against `SLY1_PART_REPLAY=0 SLY1_GPU_SPRITES=0`: mean 0.1, at most 25 of
255, at most 0.24% of values further than 8, in a strip at the screen's sides (triangles across the
view's edge, which the program's clipper cut and the GPU cuts). `--verify-levels` with
`SLY1_PART_JOBS_CHECK=1`: all 46 levels, 294,429 jobs against 333,185, 1 wrong (caught).

## The GS thread's profile, palettes, and the waterfall's splashes (2026-11-23)

What a part drawn again costs the GS thread, split by `perfgs:` (A Stealthy Approach on the
Switch, 6.4 ms a frame): its batches 3.8 ms, its register writes 1.7, its prep 0.6, the GS held
before it 0.1. The render thread now makes the preps the helper has not come to when its queue is
empty or it waits for the GS thread (`SLY1_PART_PREP_RENDER=0`: not): it made more than the helper.

A palette's load waits until something reads the palette (`ClutSettle`: a texture decoded or
hashed, or the GS's memory about to change): the renderer finds most textures without either, and a
kept part loads a palette with each TEX0 write it replays. The register writes went from 1.83 to
0.85 ms a frame on the Switch (`SLY1_CLUT_LAZY=0|ab`). `SLY1_CLUT_LAZY_CHECK=1` also reads each
load at once into a palette of its own and compares it at every textured run: all 46 levels,
16,777,216 runs, none with another palette.

`SLY1_PROFILE=1` samples the GS thread on the Switch: a thread of its own pauses it about 1,300
times a second and reads its program counter and link register (`prof:` lines, offsets from the
program's base); `tools/prof_resolve.py LOG ELF [--lines NAME]` names them from the ELF of the
build that ran (a later build's addresses differ). It found, and these were cut: the map of kept
batches' all-black answers (3.8%: the answer is now in the kept batch's record), three switch
checks that were calls (1.1%: inline), the GS thread's counters as atomic read-modify-writes (0.9%:
relaxed stores, one writer), the hash of a texture's pages as one chain of multiplies (1.8%: four
lanes). Still there: waits on memory where the GS thread walks a part's items, and copies of
batch records. `SLY1_PREFETCH=ab` compared prefetching 8 KB of the next item with 1 KB: 8 KB
stays. No kept batch follows another with nothing between (`SLY1_FUSE_SURVEY`): each has its
register writes before it, so batches cannot be joined as they come.

`perfdraw:` (each 60 frames) counts the GPU renderer's batches, kept ones drawn, black ones left
out and draws submitted, and with `SLY1_GPU_TIMERS=1` times DrawBatch, a kept batch's Layer, Submit
and Begin: about 1,060 batches a frame at about 3 microseconds each, the cost spread thin.

At A Stealthy Approach's waterfall the GPU took 16.5 to 17.3 ms a frame. `SLY1_GPU_VARY=2` (the
player standing still): without the runs into the effects' half-size picture (the splashes, about
ten screens of them in 27 runs, `gpuarea:`), 11.4 ms. That picture was half the whole image each
way whatever the frame's dynamic scale; it is now half the frame's size as drawn
(`SLY1_FX_SCALED=0|ab`), and gs_fx_fsh.glsl reads the four depths under a pixel with one
textureGather, not four texelFetch calls. In a played run with `SLY1_FX_SCALED=ab`: 96% of 3D
windows at 60 against 90%, the GPU 11.7 ms a frame against 13.0, windows with the GPU over 13 ms 31
against 98; the splashes looked right, and no change was seen between the two.

The first-start "save bug" was the game's own: pressing Start in the opening (the movie, then the
Paris rooftop scene, the element stack's state 3) begins a new game; a save is continued from the
title's menu. `cardstate:` lines log the card's and the title's states. Saves are now copied before
each write (`port/platform/save_backup.cpp`, to `save-backups/`: first/, sessions/, recent/).

### Late frames, the parts' jobs and the helper (2026-11-23)

`lateparts:` (a frame later than 20 ms: the jobs each thread ran and the render thread's wait).
In a played run of A Stealthy Approach on the Switch, 471 late frames in play: about 60% held up by
the GS thread (16.8 ms, about 345 parts drawn again, 2.7 ms of it running jobs again that the helper
had begun and then ended first), about 35% by the render thread (17.9 ms, parts in full 4.3 ms, 43
jobs). The helper's profile (`SLY1_PROFILE=helper`): `PartHasher::Feed` 23 to 26% of its samples,
most of it the reads of the parts' codes from memory.

Tried and measured without a gain: a job taken over when its run stops giving items
(`SLY1_JOB_BEAT=1|ab`, off by default: the GS thread's time on jobs 1.97 against 1.99 ms a frame)
and an unpack's words hashed in two lanes (`SLY1_HASH_LANES`, on: the same parts kept, no change in
the helper's share). Telling the game a list is finished before its walk ends stays out of reach
(the game builds into two packet buffers in turn, reused at its frame-done semaphore, and
PackFrameColors writes into the list while it is walked). Particles drawn by the renderer are not
begun: each particle may set its own TEX0 from the frame table (`SLY1_STREAK_DUMP`).

### Fewer parts hashed (2026-11-23)

The helper hashed every part drawn again from its cache's packet each list (a quarter of its time on
the Switch, reading the packets from memory), and 99% of those hashes found nothing changed. The
game writes a cache only while it relights its part the long way (its header's mode 3, the only
drawing that hands PackFrameColors its colour records): the port, at such a drawing, marks the
memory that can then be written (the cache's two packets, and each mesh record's colour blocks, f00
quadwords at f04 and f0C at f10, with the pointers at +0x08 and +0x14 that PackFrameColors writes
through) in blocks of 256 bytes, kept for 40 lists. A part drawn from its cache keeps the blocks its
packet lies on and names (`PartPages`, made once when kept); its hash is not asked again while none
of them is marked, but every 8th list (by its key) all the same. Marks are by memory, not by cache:
a colour block may be another cache's too (marking only the cache's own packets missed 20 changes
over the 46 levels). `SLY1_HASH_SKIP=0`: every part hashed; `=check`: every part hashed, and a
hash that differs where no mark was is said (`hashskip:`).

All 46 levels, 300 frames each with the camera turning, `=check`: of 2,630,908 hashes 1,819,924
(69%) were asked with no mark, 2 of them other than the hash before (one place of one level, a write
not yet found: the 8th list catches it). A Stealthy Approach's tour: the helper hashed 105,187 parts
in 1.02 s against 259,868 in 2.08 s, parts drawn again and stale by hash as before, the render
thread's time the same. `SLY1_DIFF_KEY=hex` prints what differs in one part's packet from list to list.

### Jobs run ahead, and a part whose shape turns (2026-11-23)

In the Switch's late frames held up by the GS thread (run 14 of A Stealthy Approach: 177 of them,
the GS thread busy 17.5 ms), the GS thread also spent about 2.4 ms running jobs again that the
helper had begun: the helper, below the game's threads on core 0, was set aside in the middle of a
job; after `JobWaitNs` the GS thread ran the job itself, and the helper, let go on, ended it first.
That time is not in the GS thread's busy time (`perfgs` counts items, not the waits before them).

- **Jobs run ahead.** A thread that needs a job another thread has begun and not ended now runs the
  list's next job no thread has taken (`PartJobTake`, by number, so the list's order is kept) and
  looks again; only with none left to take does it wait, then run the job again. A job run so is
  never lost: its items are the list's. `SLY1_JOB_AHEAD=0`: off; `=ab`: on and off in turn, 600
  frames each. `perflost:` (with `SLY1_PERF_LOG`) gives the runs lost by each thread a frame and
  the jobs run ahead.
- **A part whose shape turned** (its packet's shape other than its record's) was run on the render
  thread in full and watched the next time: in the Switch's late frames held up by the render
  thread such parts are the costliest of those run in full (about 0.5 ms each). It is now made a
  job at once when another part's registers for its new shape are known (`g_shapeRegs`, as for a
  part met for the first time). `SLY1_PART_SHAPE_TURN=0`: off; `=ab`: in turn, 1,200 frames
  each, so that with `SLY1_JOB_AHEAD=ab` one run has each of the four.

On the Mac the helper is not set aside, so `SLY1_HELPER_STALL_US=N` sets it aside for N
microseconds in one job of eight, after the job is begun. A Stealthy Approach's tour with 300: the
GS thread's runs lost 618 (0.22 s) against 1,263 (0.28 s), the render thread's wait at its lists'
ends 0.16 s against 0.25 s, 24,946 jobs run ahead. Without the stall, the shape turn: parts run in
full because their shape turned 0.05 a frame against 0.14 (319 parts made jobs instead), 0 jobs
wrong.

Checked: `SLY1_PART_JOBS_CHECK=1` compared a batch's record word for word, so it counted every lit
batch as different (its `lit` pointer is into each run's own buffer, and the padding beside the
one-byte fields is left as it is): it now compares the record field by field, without its pointers
and what the GS thread fills in. What it then found was older than these changes, the same with them
off: a lit batch's record carried, past its count of lights, the lights of the thread's last setup
(`t_lit`, per thread), so a job's words differed from the render thread's run of the same packet.
Nothing reads those lights; they are now cleared when the record is written. The parts not made
shapes (`perffull`'s "no shape") are those whose packets raise an interrupt (the colour relight) or
load a microprogram: they stay the render thread's.

On the Switch (2026-11-23, A Stealthy Approach played, f40dd8f): run 15 with `SLY1_JOB_AHEAD=ab` and
`SLY1_PART_SHAPE_TURN=ab` together, windows in 3D by the four turns:

| jobs ahead | shape turn | windows | at 60 | late frames in 3D per 100 windows | GS thread's runs lost |
| --- | --- | --- | --- | --- | --- |
| on | on | 80 | 92% | 90 | 1.52 ms a frame |
| on | off | 87 | 77% | 117 | 0.97 |
| off | on | 86 | 85% | 94 | 2.21 |
| off | off | 80 | 84% | 131 | 1.74 |

The shape turn takes about a quarter of the late frames away (the parts run in full for a turned
shape 0.02 to 0.03 ms a frame against 0.11); jobs run ahead take about 0.7 ms of runs lost from the
GS thread, which still loses some (a job begun elsewhere with none left to take). Both stay on. The
GS thread's profile (`SLY1_PROFILE=gs`) now shows its share of the jobs' work (LitColours,
LightFourWide, SkinLoop: about 3%), and the prefetch of an item's words 2.3%.

Run 16, every part hashed (`SLY1_HASH_SKIP=0`), the rest as default, the same stretch played: 86% of
264 windows in 3D at 60 against run 15's 92% of 80 with both on, 94 late frames per 100 windows
against 85, the helper's job 340 against 333 us. Two runs on routes that differ: the skipping of
hashes stays on, with no more than a small gain seen.

## A kept part's commands kept (2026-11-24)

The first step of drawing model parts natively (the decision of 2026-11-21: a native draw path for
the parts, measured against the translation). In the Switch's late frames held up by the GS
thread, drawing kept parts again was 9.5 to 11 ms of its 17.5: each batch of each part went through
the GS's run state (`RunState`, `Begin`, the texture's lookup), then the renderer's (`DrawBatch`,
`Layer`, `Submit`: blend, tests, fog, scissor, the texture's binding, the batch's numbers, the
streams, one or two draws), the same every frame but for the part's numbers.

Now (`SLY1_GPU_PART_LISTS=1`, or `=ab` on and off in turn; off by default until measured on the
Switch) a kept part's commands are kept as a deko3d command list of its own:

- **Recording.** The first time a part comes with a key (below), its batches are drawn as ever,
  but into a command buffer of its own (`listCmd_`), from a state where nothing is taken as bound
  (`ResetTracking`), and each batch's numbers are bound from a slot of their own (256 bytes each in
  `paramMem_`) instead of being pushed into the one buffer. At its end the slots are filled for this
  frame (`dkCmdBufPushConstants`, in order with the draws) and the list is called from the frame's
  list (`dkCmdBufCallList`). A recording that met anything a list cannot hold (a run of the GS's,
  vertices or colours of this frame, a small target, the effects' picture, DATE, a texture decoded,
  commands sent, more than 256 slots, anything else drawn or uploaded meanwhile) is called this
  once and not kept; the part is tried again after 240 frames, then less often.
- **Giving again.** While the key is the same and the list's textures are what they were (looked
  at again only when anything has been uploaded since: their pages' generation, decoded again in
  place where it changed, the same texture with the same alpha range), the part's batches do not go
  through the GS's run state nor the renderer's: `gs::PartReplayBatch` takes each batch's tag (PRIM)
  and XYOFFSET as `DrawBatch` would, the renderer makes the batch's numbers (`MakeBatchParams`, the
  arithmetic `BindSource` had), pushes them into the slots and calls the list. The part's register
  writes are made as ever (the GS's state, its palettes, are what they would be).
- **The key** is the item's (the part's batches' keeps, the TEX0 writes put right, the viewport's
  depths, the alpha's scale where a batch takes it), the registers' as the part comes (but those
  the part writes before its first batch, noted the first time: their values as it comes are never
  seen), the image's size (dynamic resolution) and the faces dropped. A part keeps two lists, one
  for each frame buffer (FRAME differs). Every list is forgotten when the textures' cache, the kept
  batches' memory or the lists' own memory (48 MB, a reserve of 4) is made anew.
- **This frame's vertices and colours** (a part the helper prepared: lit, swayed, skinned): the
  recording puts them at places of the list's own (`listData_`) instead of the frame's memory, and a
  list given again has this frame's copied there first (`PartEnd`). It is given only when the GPU has
  done with the frame that last drew from them (`DataFree`: three frames since, the sets of the
  frame's memory being three, or that frame's fence signalled); else the part is drawn as ever that
  frame (`perflists`' "data in use"). Which of its batches have them is in the key. While recording, a share that adds this frame's colours is drawn even when they are all
  black (left out otherwise): adding black changes nothing, and the list holds for any frame's.
- **The alpha's scale**: a part whose batches take it is recorded with no alpha test decided by its
  vertices' alpha (two draws where one would do, the same picture), so that its list holds for any
  scale; a fading part is given from its list too.
- **What is never offered**: a part lit, swayed or skinned anew each frame without the helper
  (`BatchNow` on the GS thread; none in the tour).

Measured on the Mac with a stand-in for the renderer (`SLY1_GS_COUNT_PARTS=1`: kept when a key is
new, given again when it is not). All 46 levels, 300 frames each with the camera turning: of 207
parts offered a frame, 195.6 given again (94.5%, 1,359 batches a frame that no longer go through
the GS's and the renderer's state), 11.3 kept anew. The alpha's scale in every key had made 10% of
the parts' frames new keys (`SLY1_PART_KEY_WHY=1` says which of the item's numbers changed, and why
a part was not offered).

**In Ryujinx** (A Stealthy Approach, the camera turning, `SLY1_PAD_TURN=300`): with
`SLY1_GPU_PART_LISTS=check`, 453,883 parts checked over 3,000 frames, none drawn otherwise; with
`=ab` (on and off by turns of 600 frames, a frame captured in each), 172 to 256 parts a frame given
from their lists in the on turns and the pictures as in the off turns. Two faults were found on the
way. A channel's kickoff takes 512 entries at most (Ryujinx refuses more; deko3d then stops the
program at `dkQueuePresentImage`), and each list called takes two or more: the frame's commands
are submitted and flushed every 64 calls (`ListCalled`), at most 132 entries a kickoff since.
Ryujinx's GPU also faulted on a push of a whole list's slots at once (up to 64 KB); they are pushed
4 slots (1 KB) at a time. deko3d's checked build names a fault where the release build only stops:
`cmake -DSLY1_DEKO3D_DEBUG=ON` links it, and wraps the channel's kickoff to print the entries it
carried and its result (`port/switch/nv_wrap.cpp`). With it, the renderer's other pushes were found
to give their uniform buffers' sizes unaligned; they give them rounded to 256 bytes now.

For the Switch, `SLY1_GPU_PART_LISTS=check`: every part that could be given
from its list is recorded again instead, and the two recordings' signatures compared: everything
the commands are made from, draw by draw (each draw's state, its streams' places, the decisions
taken for it, its textures' places and alpha ranges, its depths, the image's size). `perflists:`
counts them ("checked the same", "otherwise"), and `partcheck:` names the first parts drawn
otherwise. The same means a list given would have drawn exactly what the batches drew.

**Memory.** Lists are recorded into twelve chunks of command memory, each a command buffer of its
own; a chunk is cleared once every list in it is let go and the GPU has done with them (three frames:
the sets of the frame's memory), and with none to clear, the chunk with the fewest lists alive has
them let go (their parts are recorded again as they come). A list's slots and its data are blocks
taken from their memory and given back when the list is let go, free again three frames after.
A list is let go when it is found stale, when its part is recorded anew, and when a kept batch of
its is let go (each list notes its batches' keeps and finds them still kept before it is given).
`perflists` gives the lists alive, those let go, the chunks emptied, the recordings with no room, and
the slots' and data's memory free.

**The key checked on the Mac** (`SLY1_GS_COUNT_PARTS=check`: a part that comes with a key it came
with before is drawn as ever by the stand-in, and the states its batches are drawn by, batch by
batch, with their layout, keeps, kicks and what of them is this frame's, compared with those it was
kept with): all 46 levels, 300 frames each with the camera turning, 2,721,001 parts checked, none
drawn by other states. With the key the same, the GS hands the renderer the same draws; what the
renderer makes of them is the Switch's check (`SLY1_GPU_PART_LISTS=check`).

**A safety net.** In any run with the lists on, one list in 64 that could be given is recorded again
instead and the two signatures compared, as the check mode does for every one; a list found other
than its part's drawing turns the lists off for the rest of the run (`perflists: ... the lists are
off`), so that a fault the Mac could not show costs a moment, not a run.

## Memory from level to level (2026-11-24)

Ryujinx's tour of all the levels (`--verify-levels`, 600 frames a level, the camera turning, the
arrival points) ran out of memory: at the 39th, then, as each cause was taken away, at the 28th and
the 26th (the heap's growth had been hiding behind each other cause). `--verify-levels` now prints,
after each level, the heap in use and taken (mallinfo on the Switch, malloc_zone_statistics on the
Mac) and `memreport:` / `gpumem:` (the tables that live from level to level). Four causes, all older
than the part lists:

- **A part whose keeping was refused kept its items** in its record, in no count and out of reach of
  the letting go for room. Let go now.
- **The part jobs' items' buffers only grew** (a run's are swapped in from the thread that ran it).
  When a job is made anew, those past 128 KB in all are let go (`SLY1_JOB_TRIM_KB`). On the Mac,
  0.15 ms a frame more on the GS thread and the helper.
- **The parts' records were never let go**: about 4,000 more a level, 1,472 bytes each. Every 600
  frames, past 8,000, those not met for 30 s and holding nothing are forgotten.
- **The spare buffers' stocks kept buffers of any size.** A GIF chain's copy (a texture's upload, half
  a megabyte) went to the GS thread's stock once drawn, and stayed: 175 MB over eight levels, and
  newlib's heap in pieces (2 GB taken for 1 GB in use). The stocks take back only buffers of about
  their own size now.

Found with `cmake -DSLY1_HEAP_PROFILE=ON` (port/switch/heap_profile.cpp: the global new and delete
replaced, the bytes held counted by the four return addresses above new; build with
`-fno-omit-frame-pointer` in the release flags for the addresses to be right).

After them, in Ryujinx: 43 levels in the run's 50 minutes, the heap 607 MB at the first and 1,823 MB
in use (2,041 MB taken) at the 43rd, still growing about 28 MB a level. Left to look at: the stocks
filling to their limits (the render thread's may hold 64 buffers of up to 4 MB), the kept batches'
vectors (QueueBatch), the items VU1's kicks give (KickPath1).
