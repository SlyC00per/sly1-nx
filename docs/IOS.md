# The iPhone version

Work in progress, on the `ios` branch (started 2026-11-26). The aim is the same game on an
iPhone, from the same tree: the game code, sound, video, saves and the disc reader are shared
with the Switch version, and what is new is a renderer for Apple's GPUs and a platform layer
for iOS.

## Plan

1. **A Metal renderer** (`port/apple/gpu_metal.mm`), written and checked on the Mac, whose GPU
   is of the same family as the iPhone's. In progress: see below.
2. **A window on the Mac**, so the Metal picture can be seen and played there.
3. **The iOS layer** (`port/ios/`): the app, the disc image found in the app's folder, saves,
   a controller through Apple's GameController framework, suspension when the app leaves the
   screen.
4. **The Xcode project and the first run on a phone.** Needs Xcode, which the Mac this is
   written on does not have yet.

Not decided yet: the picture's shape. The phone's screens are not 16:9; the port's wide view
can be made the screen's own shape instead of letterboxing.

## The Metal renderer

It takes what the GS is given to draw through the same interface as the Switch's renderer
(`port/include/port/gs_backend.h`): runs of transformed primitives with the registers they are
drawn by. The frame's two buffers and the small buffers the game draws into are images, as on
the Switch.

The pixel pipeline is not the Switch's. Apple's GPUs give a pixel shader the colour already in
the frame under its pixel, so everything the GS does after its rasteriser is done in the shader,
in the GS's own integer arithmetic, following the reference (`port/render/gs.cpp`'s `Pixel`)
line for line: the texture function, the fog, the alpha test and what a failing pixel still
writes, the destination alpha test, the blend, FBA and the frame's write mask. There is no
blend state and none of the approximations a fixed blend needs (the Switch's renderer has
several, each found by a wrong picture).

What it does not do yet:

- **Batches** (`DrawBatch`): the game's vertices are transformed by the translated VU1
  programs on the CPU, not by a vertex shader. Right, and slower than the Switch's path.
- **Lines at a higher resolution.** Metal draws lines one pixel wide; the Switch's renderer
  widens the characters' outlines for 720p. To do before the picture is upscaled.
- **A picture on a screen.** It draws into images and reads them back; there is no window.
- The Switch renderer's savings (parts kept from frame to frame, the effects at half size,
  the dynamic resolution). They may not be needed on this hardware.
- REGION_REPEAT texture wrapping (as REPEAT; not met in the levels run so far) and 16-bit
  frame buffers (not met either). Both are counted in the run's report.

## Checking it against the reference

The Mac's build has the reference drawing (`--dump-fb`: the GS's primitives rasterised into
its memory as the GS does). With a renderer attached too, both draw the same primitives, so
each frame the Metal renderer shows can be compared with the reference's picture of the same
frame, pixel by pixel:

```sh
SLY1_METAL=1 SLY1_METAL_COMPARE=1 SLY1_GS_HALF_LINE=1 SLY1_LEVEL_FRAMES=240 \
    build-fast/sly1_headless --iso DISC.iso --levels 5,20 --dump-fb DIR --dump-every 160
```

- `SLY1_METAL=1`: the Mac's build draws with Metal (without it nothing changes there).
- `SLY1_METAL_COMPARE=1`: the images at the GS's own size (640 by 224), textures filtered as
  the GS's registers say, the game's motion blur drawn (the port leaves it out; the reference
  has it), and each frame compared. `SLY1_METAL_COMPARE_EACH=N` prints every N-th frame's
  numbers; the run's report has the totals.
- `SLY1_GS_HALF_LINE=1`: the odd field's half line kept in what the renderer is given, as the
  reference has it.
- `SLY1_METAL_DUMP=DIR` (`SLY1_METAL_DUMP_EVERY=N`): the Metal frames as `metal-NNNNN.png`,
  beside the reference's `frame-NNNNN.png`.
- `SLY1_METAL_SIZE=WxH`, `SLY1_METAL_FILTER=gs|linear`, `SLY1_METAL_BLUR=0|1`: the images'
  size, the textures' filter and the blur, when not comparing.

The comparison means nothing while a movie plays: a movie's pictures go to the renderer
directly and never reach the GS's memory, so the reference's picture is not the movie.

**First result (2026-11-26, Apple M2 Max, levels 5 and 20, 240 frames each).** In the levels,
the mean difference of a colour is 0.5 to 0.7 of 255; 96 to 97% of the pixels are within 2 of
the reference in every colour, 98 to 99% within 8, 99.5% within 32. What is left has not been
looked into yet (edges and lines, where the GPU's rasteriser and the reference's differ, are
the likely share).
