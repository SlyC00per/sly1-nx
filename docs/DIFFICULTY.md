# The Difficulty setting

The Options page of the pause menu has "Difficulty: Normal" or "Difficulty: Easy" (the
maintainer, 2026-11-25), after Show FPS. It is kept with the save, as the other settings of the
port are: bit 0x80000 of the save's option flags.

**Normal**, the default, is retail's game. **Easy** changes four levels and nothing anywhere
else:

| Level | Code | What Easy changes |
| --- | --- | --- |
| Treasure in the Depths | 0x106 | the crabs move at half their speed, before they hold a chest and after |
| Piranha Lake | 0x304 | the piranhas move at 0.8 of their speed, and there are twice as many (24 for 12) |
| Down Home Cooking | 0x307 | the chickens move at half their speed |
| Burning Rubber | 0x502 | the fire slugs move at half their speed |

A level is known by the code the game keeps for the one being played (the world's number and
the level's number in it, in the save). In every other level, and on Normal, the factor is
exactly 1 and no copy is made, so the game's sums are retail's.

## How

All of it is in `port/platform/kernel.cpp` (`port::EnemyPace`, `port::ExtraEnemyClass`) and
five named patches in `tools/patches.py`; the decompilation is not changed.

- **The piranhas, the chickens and the fire slugs are agents** (class 8 and the classes made
  from it: 0x0F, 0x11 and 0x0A in these levels). An agent moves at the velocity its state's
  animation gives: `UpdateAgent` takes it each frame, and `StepAgent` takes the clip's motion
  again while the agent follows it. Both are multiplied by the level's factor. Every agent of
  the level is slowed, so Down Home Cooking's one agent of class 0x0A is too. The animation
  itself plays at its own speed.
- **The crabs are paired objects** (class 0x42), which steer at up to a top speed their
  controller sets as the level goes on. `SteerPairedObjectWithinArea` multiplies that top speed
  by the factor. It is the one place a crab's speed is set, in every state: walking to its
  place, roaming, going to a chest, and carrying one away.
- **The second piranhas** are copies, made as the level's load ends and before its objects are
  started (`LoadWorld`, where the game makes its own extra copies): one of each object of class
  0x0F. A copy starts where its original does and they part as each picks its own way.

The speeds change at once when the setting is changed in a level. The number of piranhas is
decided when Piranha Lake is loaded: the setting changed inside the level takes effect the next
time it loads (a retry, or coming back).

## What was measured

On the Mac's headless build, each level on Normal and on Easy (`SLY1_EASY=1` or `0` sets it
whatever the save says), with a count of the objects updated and how far each moved a second of
the game's time (a hook since removed):

| Level | Enemy | Normal | Easy | Ratio |
| --- | --- | --- | --- | --- |
| Treasure in the Depths | crabs, mean | about 590 a second | about 310 | 0.53 |
| Piranha Lake | piranhas, mean | about 440 | about 345 | 0.8 |
| Piranha Lake | piranhas, count | 12 | 24 | 2 |
| Down Home Cooking | chickens, median while moving | about 630 | about 310 | 0.5 |
| Burning Rubber | fire slugs, median while moving | about 920 | about 490 | 0.53 |

The means are noisy (a crab that is hit is thrown; a slug's turns and leaps are not scaled),
which is why they are not exactly the factor. The menu was walked with `SLY1_PAD_SCRIPT`: the
item is there, and choosing it turns it over.

Not done: the four levels played by a person on Easy, on the Switch or anywhere. In
particular nobody has yet driven the skiff among 24 piranhas, or finished one of the levels.

## Test aids added with it (port/platform/pad.cpp)

- `SLY1_PAD_SCRIPT=N:MASK,...`: buttons held at the Nth pad read (a menu walked unattended).
- `SLY1_PAD_TAP=N`: the cross button tapped every N reads (a level's opening talk passed).
- `SLY1_PAD_AIM=N`: the right stick held up from the Nth read (a vehicle's gun fired).
