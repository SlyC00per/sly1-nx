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
  - a wait's unit, 1/120 s (`SLY1_SND_TICK=n`: n a second). It was read as 1/240 s; then the
    dog of the title world (bank 871394's record 25: a bark of 0.95 s chosen at random, a wait of
    225 to 245, again) barked back to back, twice in the 1.45 s the game lets it sound, where
    retail barks once. At 1/120 s the game stops it before a second; 1/60 s would do as well,
    so the unit is still a reading;
  - a sample's envelope (+0x12 and +0x14, the SPU's two words), played as the music's are
    (`Envelope`): 1,306 of the global banks' sample tones have 80FF/9FC0 (no envelope to speak
    of), the others slow attacks, falling sustains and releases of up to several seconds. A
    voice the game stops (0x15) goes into its notes' release, as a key let go; before, every
    sound was cut in a hundredth of a second. `SLY1_SND_ENVELOPE=0`: as before;
  - kind 5, another record of the same bank started (the record at +0x14, a volume of 127 at
    +8; read from all ten in the global banks, each naming a record of its bank). It is how the
    ambient programs are made: bank 871394's record 26 starts a bed (7), a round that starts one
    of three calls at random every 6 to 11 seconds (12: 13, 14, 15), and every 25 to 30 seconds
    another (11); bank 782334's record 88 starts three rounds of its own. The record started is a
    voice of the one that started it, mixed at its volume, pan and pitch and stopped with it (a
    choice of kind 25 may start one too). Before, kind 5 was passed over and these programs were
    silent. `SLY1_SND_CHILDREN=0`: passed over; `SLY1_SND_KINDS=1` prints every record's program
    when a global bank is loaded;
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

## The volume channels

The game keeps four volume channels and a fifth over them all, each a product of factors it
sets for its own reasons (`ApplyVolumeChannelFactors`), sent as command 0x09 (1,024 is 1):

| Channel | What is under it | How it is known |
| --- | --- | --- |
| 0, 3 | a global bank's sound, by the record's second byte (0 for 771 of them, 3 for 30) | a reading of that byte: the only two values, and the game's `SilenceVolumeChannel3` |
| 1 | the level's music | the game takes it to 0 when the opening's narration starts and back to 512 when it ends; an animation's events 2 and 3 switch it; the sound level's ducking scales it |
| 2 | a stream | the fifth word of command 0x2C is 2 |
| 8 | everything | taken to 0 around a level's load |

The music's channel is at 512, a half, in play. Without the channels the title level's music
played over the opening's own sound (`SLY1_SND_CHANNELS=0` leaves them out). The mix puts a
stream at 0.6 and a bank's sound and the music at 0.45, each times its channel.

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
+0x10. The two envelope words are played as the SPU plays them (`Envelope`, from the public
description of the PlayStation SPU: attack, an exponential decay to the sustain level, a sustain
that rises or falls until the key is let go, a release, each a shift and a step). Before, only the
release was played, linearly, and every note held at full volume until its key was let go: in
Prowling the Grounds several instruments' sustain falls (second words `CE6C`, `CE4C`, `CEED`: an
exponential fall of about 0.8 s) and their release is exponential, so their notes hung where the
game has them fade out. A sequence that is stopped still fades its notes in a twentieth of a
second. A note still held when its sequence ends is let go by the next sequence's end of the
same key (a note over the joint between two sequences: Prowling the Grounds' music has them);
one still held when that next sequence ends too goes into its release. Before, such notes were
never let go, and in Prowling the Grounds, Down Home Cooking, A Stealthy Approach and The Unseen
Foe a looping instrument sounded on and on, more of them the longer a level was played.
`SLY1_SND_LOG` counts the music's notes and those of no playing sequence; `SLY1_SND_AUDIT=1`
reports, for each bank loaded, its envelopes, its programs' kinds and any control message of
its sequences not read (none, over all 46 levels). `SLY1_SND_ENVELOPE=0`: the release alone, as before; `SLY1_SND_ENV=1` prints each tone's
envelope the first time it sounds.

**Sequences.** The third chunk: a count at +0x17, an offset each from +0x20 (from +0x10), and
each sequence a 0xDC-byte header (the tempo in microseconds a beat at +0x20, 480 ticks a beat at
+0x24) and a MIDI event stream: note on 0x9n, a note's end 0xDn with its key, program 0xCn, bend
0xEn, tempo and end as MIDI's meta events. All 259 sequences parse so, with nothing left over.

**The control messages** (`F0 75 ... F7`), 180 distinct ones over the game. Read from how they
are used:

| Bytes | Reading |
| --- | --- |
| `00 x S`, `02 x S` | if the sound level is at least x, or under x, statement S |
| `16 r v S`, `06 r v S`, `07 r v S` | if register r is v, under v, over v (`07 00 00 S` is used, which under 0 would never do; the title level's first loop goes on to the next only once register 1, its count of the sound level's dips, is over 2) |
| `04 ... 05` | statements as one |
| `13 r v` | register r set to v |
| `11 n` | go on with sequence n in this one's place |
| `0D r` | the same, the sequence register r names (the registers keep a sequence's number and one) |
| `12 n` | sequence n started beside this one |
| `03 n` | sequence n stopped |
| `0B n ...` | (first sequence) the rest of the part kept as block n; `0C n` does it |
| `0F g m (c lo hi)...` | (first sequence) group g: channels and the sound levels each sounds between; `10 g` gives a sequence the group, which then switches its channels whenever the sound level changes ("Sneaking", below) |
| `14 n`, `0E n` | not understood: passed over |

The first sequence is messages only, in parts ended by `F7`: it sets registers, keeps blocks
and groups and starts a sequence. A Stealthy Approach: an intro (11) into a loop of four
(1 to 4), each asking at every beat whether the sound level has reached 110 and then starting a
one-beat sequence (10) that stops them and goes on with the alarm loop (5 to 8), which at each
bar goes to a wind-down (9) once the level is under 110; registers 0 and 1 keep where each loop
was left. The values the levels are compared with (18, 19, 30, 50, 110, 119, 120) are what the
game sends as the sound level: its own level plus 20.

A message's choice of another sequence is taken at once, not at the sequence's end, and a
sequence that is playing is not started again beside itself (the calm loop asks for the
one-beat sequence at every beat while the level is high). The game's "every sound stopped"
(0x34) leaves the music: after a restart of the level the game does not start it again. A new
level's music ends the last one's. A sequence that reaches its end with no other chosen starts
again (the title level's loops have no message at their end). `SLY1_SND_LEVEL=s,v,s,v` sets the sound level to v from
second s on, for hearing the changes on the host.

**Readings to put right by ear:** velocity and volumes taken as straight factors; the channel switch as between the two levels inclusive; a release when a
sequence is stopped of a twentieth of a second; no reverb.

## Not yet

- Stereo streams, if there are any (the first one, the opening's narration, is mono 44.1 kHz).
- Unmeasured: the unit of a fade's time (0x22: a fifth of a second is used); commands 0x0E, 0x0F, 0x51 (answered
  0, their meaning not looked for yet); a stream's pan.

## A step's own wait, and the wait's unit (2026-11-24)

Every step of a sound's program may carry a wait in its word at +4, taken before the step. The title
screen's siren showed it (bank 871394's record 11, started by record 26's round): one sample eleven
times, each after the first with 402 to 412 at +4, its volume 15, 25, 30, 40, 50, 45, 40, 35, 25, 20,
15: a siren that comes and goes. All eleven began at once before, heard as one siren, once.

The same record measures the unit: the sample lasts 1.73 s and the next begins 402 later, 1.80 s and
412 as its pitch falls, so back to back is 232 a second: 240ths, one sample's end under the next's
start. A wait (kind 26) is its word at +4 as any step's, and a random part up to its word at +8; the
two had been read the other way round, which is what made a dog (record 25: 225 and up to 20 more)
bark twice in quick succession, and why 120ths had seemed right for a day.

`SLY1_SND_KINDS=all` prints every bank's records with each sample's wait, volume and length.

## The tempo retail's driver plays, and a song's first steps (2026-11-24)

Measured from a recording of the PS2 in the level 0x308 (the onsets' repeat period, to 0.01%): a
beat named 1,000,000 or 500,000 us is exact, but 750,000 us lasts 0.75190 s, 428,571 us lasts
0.42830 s and 309,278 us lasts 0.30909 s. One rule gives all five: the driver steps 240 times a
second by a whole number of hundredths of a tick, `(4167 * 100 / (tempo / ticks a beat)) / 100`,
both divisions whole. The game counts the boss's beats by the tempo named, so on the PS2 the music
falls 120 ms behind them by the end of the 80-a-minute section. The port plays the tempo named
(the maintainer's decision); `SLY1_SND_TEMPO=ps2` plays the rule's.

A song's steps at its very start (they set its registers) are run as the song starts
(`Song::RunOpening`), not at the mixer's next pass: a level loaded again starts its music and sets
the section it wants within one frame, and the opening steps run later undid that, after which the
music waited for the game and the game for the music (no music and no cut scenes after "Try
again"). `SLY1_RETRY_TEST=frames[,more[,0]]` on the host does such a reload without a player.

The boss's beat clock: `SLY1_SND_BEAT=1` prints the registers' changes, what the game is told and
each eighth beat's real time against the game's. The game's clock is clamped from below at 0.015 s
a frame, not 1/60 s (tools/patches.py): on a 60.00 Hz screen every frame is a little under 1/60 s
and the clamp made the clock gain 0.4%. A register the music sets is told to the game 50 ms later
on the Switch (`SLY1_AUDIO_DELAY_MS`), for the sound's way to the speaker; that figure is not
measured.

## Sneaking: the plucked steps and the music under them (2026-11-25)

The player's report: on a PS2 each step of a sneak along a ledge is a plucked note, and the
port seemed to have none. What the game does (sly1-decomp, `PlayPhaseCrossingSound`,
`PlaySoundWithWanderingPitch`, `UpdateSoundEmitters`): entering a ledge pushes a sound level of
-20; the level steps down one every three frames and is sent, plus 20, as command 0x27; while
it is under 0 each step also starts sound 0x77 (record 119 of the first global bank) at half
volume, a semitone or so from the last one's pitch.

The port did all of that: on the host, with the player put beside a ledge and circle tapped
(`SLY1_TOUR=ledges`), the level went to -20 and record 119 was started at every step, at about
the level of a footstep. Alone it measures 140 to 260 Hz and under a fifth of a second: a low
note, which a Switch's own speakers give little of. What was wrong was the music over it.
"Prowling the Grounds" has a group whose channels 15, 0 and 11 sound at levels 0 to 19 and
the others at 20 and over: a quiet arrangement for sneaking. The port switched a sequence's
channels only when the sequence came to its `10 g` statement, once a loop, so the music stayed
full through a sneak. Now the group a sequence was given goes on deciding: when the level
changes its channels change at once, and a channel switched off lets its notes go
(`Song::TakeGroup`, `Song::LevelChanged`). `SLY1_SND_GROUPS=0`: at the statement only, as before.

Measured on the host, the music alone, with the level set to 0 for eight seconds
(`SLY1_SND_LEVEL=22,0,30,20`): it falls from some 2,300 to some 1,100 (RMS over two seconds)
and comes back. **A reading, to be put right by ear:** that retail switches at once and not at
the statement was not measured on a PS2; and whether the pluck is then as loud against the
music as retail's is the player's to say.

`SLY1_SND_LOG` now also prints each change of the sound level, each group, and a sound's
record and tone volumes.

