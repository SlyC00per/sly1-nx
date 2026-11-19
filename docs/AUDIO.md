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

- **The global banks' sounds** (command 0x11: the bank's handle, the record, a volume, a pan,
  a pitch). A bank is read whole when it is loaded. A voice is the record's tones: a tone of
  kind 1 plays a sample (its place at +0x18, its volume of 127 at +0x09, times the record's
  first byte of 127); kind 25 is followed by a count of tones of which one is taken at random
  (one that is not a sample is silence); a record with kind 21 in it and no sample to play is
  held, silent, until it is stopped. A sample loops where its frames' flags say (bit 2 the
  loop's start, bits 0 and 1 its end). The voice's volume is 0x400 for 1, its pan an angle in
  degrees (0 ahead, 90 right, 270 left: the game's own arithmetic), its pitch 1,524 an octave
  (the game multiplies its own pitch by 1,524, which is 12 times 127), and a stop takes a
  hundredth of a second.

  **Readings, not measurements** (to be put right by ear against a PS2):
  - the word at +0x0A of a tone as the sample's rate in Hz (17,066 for most, about 31,920 for
    the rest, the second 15/8 of the first);
  - kinds 27, 20, 5, 26, 21, 22 and the others (27 comes before a sample with 100, 70, 65: a
    chance or a level; 20 stands where a pick may fall on nothing; 21 and 22 look like a
    round's start and end, 26 a wait): passed over;
  - the tone's bytes at +0x04, +0x10, +0x11, +0x16: not used;
  - how many of one sound may play at once: the opening starts two short records 48 and 38
    times a second, and every one is played.

## Not yet

- The level banks' sounds (`SBv2`): a voice is given a handle and is at once over.
- The level banks' sequences (the level music).
- Stereo streams, if there are any (the first one, the opening's narration, is mono 44.1 kHz).
- Unmeasured: the unit of a fade's time (0x22: a fifth of a second is used); the volume
  channels' effect on streams (0x09 is kept, not applied); commands 0x0E, 0x0F, 0x51 (answered
  0, their meaning not looked for yet); a stream's pan.
