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
