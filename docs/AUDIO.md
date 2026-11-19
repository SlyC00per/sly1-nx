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
  - a tone's two bytes at +0x0A as the note its sample plays at its own speed (a signed byte,
    negative) and a fine part in 128ths of a semitone, with 48,000 Hz (the SPU's rate) where
    the played note equals them, and a sound played as note 60. This comes from the level
    banks, whose tones have the same two bytes beside a range of keys: a drum's note is its key
    plus 12 (24,000 Hz), and an instrument over every key has 62 and 0x44, which is 44,094 Hz
    at key 60. With it the global banks' pairs are standard rates: (86, 66) 11,013 Hz for 761
    tones, (74, 66) 22,026, (80, 124) 15,990, (92, 98) 7,900. What is not shown is the note a
    sound is played as: 72 would put all of them an octave up (`SLY1_SFX_OCTAVE=1`). Read
    before as a 16-bit rate (17,066 Hz) and as a scale with 22,050 Hz for the commonest value;
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

## The level music

A level's bank (`SBv2`) is its instruments and its sequences; the game starts its record 0 once
(command 0x11) and from then on only sends the sound level (0x27) and eight registers (0x29).
Everything below is read from the 45 banks' bytes and the game's calls; none of it is measured
against the driver. It is in `port/audio/audio.cpp` (`Song`).

**Instruments.** A record (8 bytes) is a tone count, a volume of 127 and the place of its
tones; a MIDI program number is a record. A tone (24 bytes) is a sample over a range of keys:
volume +1, note +2 and fine +3 (as the global banks' tones: 48,000 Hz where the key equals
them), pan +4 (degrees, signed), the lowest and highest key +6 and +7, the bend's range down and
up in semitones +8 and +9, the SPU's two envelope words +0xA and +0xC, the sample's place
+0x10. Every envelope read has an instant attack and a full sustain, so only the release is
played: the low five bits of the second word, as the SPU's linear release (8 of 32,768 each
2^(rate - 11) samples).

**Sequences.** The third chunk: a count at +0x17, an offset each from +0x20 (from +0x10), and
each sequence a 0xDC-byte header (the tempo in microseconds a beat at +0x20, 480 ticks a beat at
+0x24) and a MIDI event stream: note on 0x9n, a note's end 0xDn with its key, program 0xCn, bend
0xEn, tempo and end as MIDI's meta events. All 259 sequences parse so, with nothing left over.

**The control messages** (`F0 75 ... F7`), 180 distinct ones over the game. Read from how they
are used:

| Bytes | Reading |
| --- | --- |
| `00 x S`, `02 x S` | if the sound level is at least x, or under x, statement S |
| `16 r v S`, `06 r v S`, `07 r v S` | if register r is v, over v, under v |
| `04 ... 05` | statements as one |
| `13 r v` | register r set to v |
| `11 n` | go on with sequence n in this one's place |
| `0D r` | the same, the sequence register r names (the registers keep a sequence's number and one) |
| `12 n` | sequence n started beside this one |
| `03 n` | sequence n stopped |
| `0B n ...` | (first sequence) the rest of the part kept as block n; `0C n` does it |
| `0F g m (c lo hi)...` | (first sequence) group g: channels and the sound levels each sounds between; `10 g` switches a sequence's channels by it |
| `14 n`, `0E n` | not understood: passed over |

The first sequence is messages only, in parts ended by `F7`: it sets registers, keeps blocks
and groups and starts a sequence. A Stealthy Approach: an intro (11) into a loop of four
(1 to 4), each asking at every beat whether the sound level has reached 110 and then starting a
one-beat sequence (10) that stops them and goes on with the alarm loop (5 to 8), which at each
bar goes to a wind-down (9) once the level is under 110; registers 0 and 1 keep where each loop
was left. The values the levels are compared with (18, 19, 30, 50, 110, 119, 120) are what the
game sends as the sound level: its own level plus 20.

**Readings to put right by ear:** velocity and volumes taken as straight factors; `06` and `07`
as over and under; the channel switch as between the two levels inclusive; a release when a
sequence is stopped of a twentieth of a second; no reverb.

## Not yet

- Stereo streams, if there are any (the first one, the opening's narration, is mono 44.1 kHz).
- Unmeasured: the unit of a fade's time (0x22: a fifth of a second is used); the volume
  channels' effect on streams (0x09 is kept, not applied); commands 0x0E, 0x0F, 0x51 (answered
  0, their meaning not looked for yet); a stream's pan.
