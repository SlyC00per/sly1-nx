# Reports of a fault or a hang

A run on the Switch with nobody reading its log leaves what went wrong on the SD card, in
`sdmc:/switch/sly1/crashes/` (port/switch/crashlog.cpp):

- `fault-DATE-TIME.txt` when the program faults: where, the callers, the registers.
- `hang-DATE-TIME.txt` when no frame has been shown for 30 s (`SLY1_HANG_SECONDS`; 0 for never):
  written once a stall, eight a run at most, and the game is left to go on.

Each begins with the version and the commit of the build, the time (UTC), how long the run had
lasted, the frame, and the level last loaded; then where every other thread was, and the last
12 KB the program had printed. The times in the names are UTC.

    python3 tools/crash_report.py fault-20261125-184639.txt --elf sly1.elf

names the addresses (function, file and line). **The `.elf` must be the one of that build**: the
report's first line gives the commit, and a release keeps its `sly1.elf` beside its `.nro`.

## What it costs

Nothing in a frame. A thread that was there already (the run's deadline's) looks at the count of
frames shown ten times a second; each line the program prints is copied once more into a ring.
A report's file is written through the file system's own calls with no heap, so that a fault
inside the C library's allocator can still be written down. A hang is counted in that thread's
turns and not by the clock: with the console asleep, or the program set aside by HOME, no turn
passes.

## What it does not catch

- The whole system going down (Atmosphere's own report is in `sdmc:/atmosphere/crash_reports/`).
- A fault in the fault handler's own thread before the program's output is set up.
- A game that still shows frames but no longer answers: that is not a hang by this measure.

## Tried on the Switch (2026-11-25)

Through the receiver, with the file read back (`SLY1_CRASHLOG_ECHO=1`): a fault made on purpose at
the title and in a level (`SLY1_FAULT_AFTER`), and every other thread paused for 8 s
(`SLY1_STALL_AFTER`, `SLY1_HANG_SECONDS=3`), after which the game went on and ended as it should.
Each run gave the process back clean. Not tried: a start from the homebrew menu's list, and a
fault inside the allocator.
