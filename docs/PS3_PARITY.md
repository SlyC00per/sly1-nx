# Matching the PS3 version

**The Switch port matches the PlayStation 3 release of the game as closely as possible**
(maintainer's direction, 2026-11-14). Where the PS3 release differs from the PS2 original,
the port follows the PS3 release; everywhere else, PS2 retail (which the decompilation
reproduces exactly) stays the reference. A difference from PS2 retail that is not listed here
is a port bug.

| Area | PS2 retail | The port, as the PS3 release |
| --- | --- | --- |
| Aspect ratio | 4:3 | 16:9 widescreen (Hor+): projection and culling widened, 2D elements anchored without stretching |
| Textures | PS2 textures | HD textures in place of the PS2 ones |
| Saving | memory card, with its checks, slot selection and prompts | no memory card: progress saved to the console's save data, none of the card screens |
| Resolution | 640×448 | HD output: 1280×720 handheld, 1920×1080 docked |
| Frame rate | 60 Hz | locked 60 FPS |
| Touch screen | none | as the PS Vita release: the screen touched anywhere is L1 (the Binocucom), in handheld play |
| Options | "Vibration" and "Speakers" settings | neither: the sound is stereo, and the vibration is on for a console with motors (the maintainer's choice, 2026-11-24; not checked against the PS3 release) |
| Options | "Center Screen..." moves the picture on the television | no such item: the screen's picture is where it is (the maintainer's choice, 2026-11-24; not checked against the PS3 release) |
| Colour | the PS2's picture | Options has "Picture: Normal" (the PS2's picture, the default) or "Picture: Vivid": each colour moved away from its grey by 1.35 as the frame goes to the screen. Measured on "At the Dog Track" against PS3 screenshots, the port's exact pixels match the PS3's sky and far road within a few units; near textured surfaces are more saturated on the PS3 (the near road 69% against 37%), which its HD textures may explain. Vivid is an approximation: over four regions it takes a quarter off the difference, and no single factor does better |

Further PS3 differences are added to this table as they are observed. PS3 platform features
with no Switch equivalent (trophies, stereoscopic 3D) are not planned unless the maintainer
asks.

## Status

- **The Binoc-u-com in widescreen** (2026-11-23). As the PS3 release has it, the lens and its blue
  frame are stretched across the 16:9 picture, while the dialogue text and the two portraits keep
  their shape. The caption box (screen element 1) is as wide as the screen, so `port::Wide2D`
  leaves it stretched and narrows only what it draws after the lens (the text, its box, the button
  glyph). Each portrait's character is a model placed where its box shows it, and the box's black
  and scissor are drawn by requests queued for later; all three are now narrowed as the box is
  (`port::WideAt`, `port::WideScope`). Before, the frame left the black it borders, and the
  characters stood out of their boxes over a black wider than them. `SLY1_WIDE_LOG=1` lists the
  elements drawn, with their places and widths.
- **Saving** (2026-11-15). The port answers the memory card library's requests from a folder of
  the player's (port/platform/memory_card.cpp; `--save FOLDER` on the host): an 8 MB card in
  port 0 that is always there and formatted, so the "no card", "unformatted" and "card full"
  pages never come. Where the game would ask whether to create its save file, it creates it (a
  named patch in tools/patches.py). The game's own save code writes its files there as on a
  card (its directory, `icon.sys`, the icon, three slot files) and reads them back next time.
  Left: the messages the game shows while saving still name the "Memory Card (PS2)"; how the
  PS3 release words or hides them is to be observed on the maintainer's copy. On the Switch the
  folder goes to the console's save data (milestone 2).

## Sourcing

- The PS3 release is a reference for **behaviour**, observed on the maintainer's own copy. No
  PS3 code or executable bytes are copied, and a PS3 behaviour is not implemented from memory:
  observe it, then record it here.
- The HD textures come from the maintainer's own copy at run time and are never committed.
- The changes live in the port's own layer and patches, never in `sly1-decomp/`.

## Beyond the PS3 release

- **Camera: Normal / Inverted** on the Controls page of the pause menu, as the PS Vita release
  has (tools/patches.py; bit 0x10000 of the save's option flags, set for Inverted). Normal, the
  default, turns the camera by the right stick the way retail does on a PS2 (the user's
  measurement, 2026-11-18); before the setting the port turned it the other way, and why it
  differed from retail there was not found (the stick's values reach the game in the PS2's
  convention). Inverted is that earlier direction.
