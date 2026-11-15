# sly1-nx

A native Nintendo Switch homebrew port of *Sly Cooper and the Thievius Raccoonus* (PS2, NTSC-U
`SCUS_971.98`), built from the matching decompilation
[sly1-decomp](https://github.com/OpenThievius/sly1-decomp), which this repository takes as a git
submodule at `sly1-decomp/`, pinned to a commit.

## The game's data is not here

This repository holds only the port's own code, build files and documentation. The game's data
comes from your own disc: the port reads it from your own `.iso` image at run time. No disc
image, executable, extracted asset, texture, compiler or SDK file is in this repository, and
none is ever committed to it.

## Getting the source

```
git clone --recurse-submodules https://github.com/SlyC00per/sly1-nx.git
```

## Documents

- `docs/BRIEF.md`: the port's task brief and milestones.
- `CLAUDE.md`: the rules and commands for agents working here.
- `sly1-decomp/docs/PORTING.md`: where every PS2-specific part of the game is.

## Licence

AGPL-3.0-or-later; see `LICENSE`.
