// SPDX-License-Identifier: AGPL-3.0-or-later
// Reports of a fault or a hang, written to the SD card (port/switch/crashlog.cpp, docs/CRASHES.md).
// A run with nobody watching its log leaves in sdmc:/switch/sly1/crashes/ what the log would have
// said. Nothing here runs in a frame: the cost in play is a copy of each line the program prints.
#pragma once
#if defined(__SWITCH__)
namespace port::crashlog {

// Once, after the program's output is set up: the folder, and the last lines printed kept.
void Start();
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
