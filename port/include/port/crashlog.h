// SPDX-License-Identifier: AGPL-3.0-or-later
// Reports of a fault or a hang, written to the SD card (port/switch/crashlog.cpp, docs/CRASHES.md).
// A run with nobody watching its log leaves in sdmc:/switch/sly1/crashes/ what the log would have
// said. Nothing here runs in a frame: the cost in play is a copy of each line the program prints.
#pragma once
#if defined(__SWITCH__)
namespace port::crashlog {

// Once, after the program's output is set up: the folder, and the last lines printed kept. What
// the program prints goes on to where it went before only with `forward`: a link is there to read
// it (nxlink, the receiver). Started from the console's own menu nothing is put out at all: the
// lines are kept for a report and go nowhere else.
void Start(bool forward);
// The same choice, changed later (SLY1_QUIET's trial; a report's echo).
void Forward(bool on);
// A report begun: what build, when, how long in, the frame and the level. `kind` is "fault" or "hang".
void Begin(const char* kind);
// A line of it.
void Line(const char* text);
// The report written, with the last lines the program had printed. Uses no heap.
void Write();
// Called ten times a second by a thread of its own kind: a report (once a stall) when no frame has
// been shown for SLY1_HANG_SECONDS (30).
void Watch();

}  // namespace port::crashlog
namespace port {
// Where every other thread is: a line each to `line` (port/platform/thread.cpp). They are paused
// for the moment it takes.
void DescribeThreads(void (*line)(const char*));
// SLY1_STALL_AFTER's trial: every other thread paused, or let go again.
void PauseOtherThreads(bool pause);
}
#endif
