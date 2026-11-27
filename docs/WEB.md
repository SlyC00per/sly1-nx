# The web version

Work in progress, on the `web` branch (started 2026-11-26). The aim is the game in a browser,
from the same tree. It is not playable yet: this page says how far it is and what stands in
the way.

## What a web version can and cannot be

- **Not a site to visit and play.** A built program holds the game's compiled code and the
  vertex programs translated from the disc, as a built `sly1.nro` does, and none is published
  (README, "The game's data is not here"). A web version is built by its player from their own
  disc image and served from their own computer; the disc image is chosen in the page.
- **64-bit WebAssembly with threads.** The port keeps words that are not memory in a native
  pointer's upper bits and takes any pointer below 4 GiB for a plain word (`port/ptr32.h`), so
  a pointer must be 64 bits: the build is `wasm64`, its data starts at 4 GiB, and its memory is
  5 GiB of address space, of which the part below 4 GiB is never touched. A browser must have
  64-bit memories and shared memory (a page served with the cross-origin isolation headers).
- **The PS2's float rounding is not available.** The game's threads round toward zero and
  flush denormals on the other systems, as the PS2's FPU does (`port/platform/float_mode.cpp`).
  WebAssembly has one rounding mode, to nearest. Results can differ from retail's in a float's
  last bit; what that does to the game has not been looked at.

## Building (so far)

```sh
emcmake cmake -B build-web -G Ninja -DPLATFORM=HOST -DCMAKE_BUILD_TYPE=Release \
    -DSLY1_ISO="/path/to/your/Sly Cooper.iso" -DSLY1_WEB_NODE=ON
cmake --build build-web --target sly1_headless
node build-web/sly1_headless.js --iso DISC.iso --save DIR --seconds 30
```

Emscripten 7.0.1 (Homebrew), node 26. `SLY1_WEB_NODE=ON` is the program for node, reading the
disc image from the computer's files: the first aim is the game running there, with no picture,
before anything of a browser's.

## State (2026-11-26)

The whole program compiles and links (a 6 MB `.wasm`), starts under node, runs its
constructors, starts the game's threads, and stops at the first call through a stored function
pointer whose type is not the called function's: `function signature mismatch`, in the
interrupt dispatch.

That is the main obstacle. On a PS2, and on the other systems the port runs on, a function
called through a pointer of another type still runs: the arguments are in registers either
way. WebAssembly checks the type at each such call and stops. The game has many (a method
called with an argument more or fewer, an integer where the function takes a pointer, a result
ignored). What was done for the ones the linker itself reports:

- `PostSemaphore` and `AwaitSemaphore` take the id as an integer, as every file of the game
  declares them now (`port/platform/kernel.cpp`); `GetDmaChannel` returns a pointer.
- Two functions that two files declare `void` and that return a pointer are declared with the
  definition's type (two patches in `tools/patches.py`).
- Two linker leftovers the game names as bytes are bytes in this build.
- A cast to a function pointer's typedef keeps the typedef's name in the generated code
  (`tools/gen_pointers.py`), so that `long` and `long long` do not part ways.

Emscripten's own answer (`EMULATE_FUNCTION_POINTER_CASTS`) does not build here: Binaryen's pass
stops on an assertion with this program.

## What is next

1. **Calls through stored function pointers.** Either each mismatch is put right at its source
   (sly1-decomp's `tools/call_types.py` lists one kind), or the generated code calls every
   stored function through one calling convention that hands over arguments as the PS2's
   registers do (integers in order, floats in order), which makes every mismatch behave as on
   the PS2 at once. The second is the plan.
2. The game running under node to the first level, compared with the Mac's run.
3. The browser's parts: the disc image from a chosen file, saves in the browser's storage,
   sound, the keyboard and a controller.
4. A renderer. WebGL 2 and WebGPU do not give a pixel shader the colour under its pixel, which
   the Metal renderer is built on, so it is the Switch renderer's method (blend states) written
   for WebGL 2, or the reference drawing on the CPU as a first picture.
