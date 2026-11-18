# Sound

The engine is `port/audio/` (port/include/port/audio.h); the design is docs/DESIGN.md, section
8. What a command means is taken from the game's own code (sly1-decomp
`src/main/iop_commands.cpp`, whose comments name each command, and `sound_system.cpp`) and
from the disc's bytes (sly1-decomp `docs/DISC.md`, "Sound banks"). The driver is never read.

## The commands

The game sends commands to two services of the IOP (`port/platform/iop.cpp`):

- **0x123456**, the sound commands. One sent alone is answered with one word between two
  marker words; a buffer of them (service command 0x4D: a count, then each as two halfwords,
  number and length, and its data padded to four bytes) with one word each.
- **0x123457**, command 3: a sound bank loaded from its first sector, answered with a handle.

`SLY1_SND_LOG=1` prints each command with its words (the first 40 of each).

## What plays (2026-11-18)

- **Streams** (command 0x2C: the first sector of a VAG file, its volume in the high half of the
  third word, 0x400 is 1). The file is `VAGp` with the size and the rate big-endian at 12 and
  16 and the SPU's ADPCM from 0x30; it is read from the disc as it plays, decoded, brought to
  48 kHz and mixed. Paused, resumed, stopped, its volume set and asked whether it still plays
  (0x13, 0x14, 0x15, 0x1B, 0x19).
- The mixer is a thread the program starts: 48 kHz stereo to the Switch's audio output, or on
  the host to the WAV file `SLY1_AUDIO_WAV` names (written at the end).

## Not yet

- The banks' sounds (command 0x11): a voice is given a handle and is at once over.
- The level banks' sequences (the level music).
- Stereo streams, if there are any (the first one, the opening's narration, is mono 44.1 kHz).
- Unmeasured: the unit of a fade's time (0x22: a fifth of a second is used); the volume
  channels' effect on streams (0x09 is kept, not applied); commands 0x0E, 0x0F, 0x51 (answered
  0, their meaning not looked for yet); a stream's pan.
