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

Further PS3 differences are added to this table as they are observed. PS3 platform features
with no Switch equivalent (trophies, stereoscopic 3D) are not planned unless the maintainer
asks.

## Status

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
