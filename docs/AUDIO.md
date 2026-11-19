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
  a pitch). A bank is read whole when it is loaded. A voice runs the record's tones as a program, a step each: a tone of
  kind 1 starts a sample (its place at +0x18, its volume of 127 at +0x09, times the record's
  first byte of 127) and the program goes on at once; kind 26 waits (the word at +8 and a
  random part up to the word at +4, taken as 240ths of a second); kind 25 takes one of the
  next n tones at random (one that is not a sample is silence); kind 21 starts a round and 22
  goes back to it. So a record can be one sample, several spaced out (a rattle: clicks 10 apart),
  or a bed that loops with others over it at random times (the rain of A Stealthy Approach: a
  looping sample, then one of eight drips every 200 to 275). Before, every sample of a record
  was started at once. A sample loops where its frames' flags say (bit 2 the
  loop's start, bits 0 and 1 its end). The voice's volume is 0x400 for 1, its pan an angle in
  degrees (0 ahead, 90 right, 270 left: the game's own arithmetic), its pitch 1,524 an octave
  (the game multiplies its own pitch by 1,524, which is 12 times 127), and a stop takes a
  hundredth of a second.

  **Readings, not measurements** (to be put right by ear against a PS2):
  - the word at +0x0A of a tone as the sample's pitch on a scale of equal ratios: its
    commonest value (0x42AA, 1,242 of the 1,429 sample tones) is taken as 22,050 Hz and its
    second (0x7CB0) as the octave above, 44,100 Hz, which puts the values between near 24,000,
    32,000 and 37,800 Hz. (Read first as a rate in Hz, 17,066: too low by ear.);
  - the kinds themselves (1, 25, 26, 21, 22), read from the sequences the records are made of;
  - a wait's unit, 1/240 s: with it a record of 0.03 s clicks 10 apart is a rattle, and a
    one-second creak comes back every 7 to 11 seconds; at 1/60 s the same would be four times
    as slow;
  - kinds 27 (a value before a sample, 100 for most, else 75, 50, 65: a level or a chance),
    31 (two values such as 80 and 120: a range, of volume or pitch), 4, 5, 20 and the rare
    ones: passed over;
  - a tone's volume byte over 127 (38 tones): played at 1;
  - the tone's bytes at +0x04, +0x10, +0x11, +0x16: not used;

  **Whether a sound loops** (command 0x1A, asked once for each sound when its bank is loaded).
  The game keeps the answer as the sound's "kept" flag (0x1BE268) and, for a sound with it, keeps
  the record when the driver says the voice is over and starts the voice again (0x1BFCD8): so
  the answer is whether the sound goes on until stopped, not whether the bank has it. Answered
  from the record's word at +6, which is set for the records whose samples loop (118 of the
  119 in the seven global banks) and for no other. Answering 1 for every sound made the game
  start each one-shot again as soon as it ended: 6,000 voices in the opening's first 50
  seconds, where there are 130.

## Not yet

- The level banks' sounds (`SBv2`): a voice is given a handle and is at once over.
- The level banks' sequences (the level music).
- Stereo streams, if there are any (the first one, the opening's narration, is mono 44.1 kHz).
- Unmeasured: the unit of a fade's time (0x22: a fifth of a second is used); the volume
  channels' effect on streams (0x09 is kept, not applied); commands 0x0E, 0x0F, 0x51 (answered
  0, their meaning not looked for yet); a stream's pan.
