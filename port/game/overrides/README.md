# The port's own functions

A file here replaces one whole function of the decompilation in the code the port builds
(tools/overrides.py; docs/DESIGN.md, "The port's own functions"). The decompilation itself is
never changed.

    port/game/overrides/<the file under sly1-decomp/>/<function>.cpp

for example `port/game/overrides/src/main/world_listed_object.cpp/func_0013A618.cpp`.

Each file begins with its SPDX line and a `// Why:` comment, then defines that one function,
complete, under the name the decompilation defines it by and with the same parameters and
result.

Rules (the maintainer's, 2026-11-21):

- This is for what the port must do differently on its hardware: the renderer's draw functions
  first. Gameplay code stays as decompiled, and the port's play stays what retail's is
  (docs/DESIGN.md, section 12).
- A change of a line or two is a named patch (tools/patches.py), not an override.
- `python3 tools/overrides.py list` is the whole list of where the port's functions are not the
  decompilation's. Keep each reason exact.
- An override that no longer finds its function (after the decompilation moves on) stops the
  build. Read what changed in the decompilation's function before carrying the override over.
