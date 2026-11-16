// SPDX-License-Identifier: AGPL-3.0-or-later
// Threads whose stacks lie in game memory. Every thread that runs game code needs one: the game
// links stack frames into its global lists and stores their addresses as game addresses
// (port/ptr32.h). On the host, pthread_attr_setstack gives the thread that stack. libnx maps a
// given stack to a mirror elsewhere and leaves the original unreachable, so on the Switch the
// thread starts on a stack of libnx's own and moves onto the game-memory stack at once.
#pragma once

#include <cstddef>

#include <pthread.h>

namespace port {

// Starts entry(arg) on a thread whose stack is [stack, stack + size): page-aligned, a whole
// number of pages. Returns 0 or an error number, as pthread_create does.
int StartThread(pthread_t* thread, void* stack, std::size_t size, void* (*entry)(void*), void* arg,
                bool detached);

// Where a thread runs on the Switch, which has three cores for a program and does not share a
// core by time between threads of one priority (a thread that waits without blocking keeps the
// others of its core from running): the game's threads on core 0 (one of them runs at a time,
// the others block in the port's kernel), the vertical blank's clock on core 1, at a higher
// priority, as an interrupt is, and the threads that watch the game (the verifier) on core 2,
// where the render thread (VIF1, VU1 and the GS) runs too, at the game's priority. Nothing on the
// host, whose scheduler shares the cores.
enum class ThreadRole { Game, Clock, Watch, Render };
void PlaceThread(ThreadRole role);

}  // namespace port
