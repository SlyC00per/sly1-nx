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
| Native handlers for the entry points | port/render/vu1_handlers.cpp | next |
| deko3d back end (GS primitives on the GPU, texture cache from local memory) | | after the first handlers |

## Speed

VU1 runs about 1.4 million instructions a frame. The translated code runs 150 million a second
on an Apple M2 and 11.3 million on the Switch (about 4 frames a second there): hence the native
handlers, as GPU shaders where the work is per vertex.

## How a handler is written and checked

1. Read the entry in the listing (`tools/vu1_disasm.py`), with the builder that packs its input
   (the decompilation's `placed_object_model.cpp` and the others, docs/DESIGN.md section 7):
   write down its inputs (where in data memory, relative to TOP), its outputs (memory, registers
   a later entry reads, packets kicked) and what it computes, including the order of its float
   operations and the VU's rules (port/render/vu1.cpp).
2. Write it in `port/render/vu1_handlers.cpp`, registered by chunk hash and entry.
3. Check it on the host with `SLY1_VU1_CHECK`: each call runs the handler and the interpreter
   from the same state and compares everything (registers, flags, Q, P, data memory, the
   packets kicked), reporting the first difference. The interpreter's result is kept, so a
   wrong handler cannot change the run it is checked in. A handler is done when every level
   runs through its check without a difference.
4. Then move its per-vertex work to the GPU where that pays (a vertex shader computing what it
   computes), checked the same way through the packets.
