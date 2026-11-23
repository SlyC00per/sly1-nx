// SPDX-License-Identifier: AGPL-3.0-or-later
// The platform layer's answers to the EE's system state (port/ps2/system.h uses them).
#ifndef PORT_SYSTEM_H
#define PORT_SYSTEM_H

#include <atomic>
#include <cstdint>

namespace port {

// The COP0 Status register as the game sees it: interrupts enabled (EIE, bit 16).
std::uint32_t Cop0Status();
// The EE's cycle counter: 294,912,000 counts a second, from the port's clock (in a deterministic
// run, from the frame count).
std::uint64_t CycleCount();

// SLY1_LOCKSTEP (port/host/main.cpp): the game's time made of the vblanks delivered, not the wall
// clock, so that every run of the same build sees the same frames. LockstepTick counts a vblank;
// CycleCount then counts from them (and a little at each call, so a wait on it ends).
void SetLockstep(bool on);
void LockstepTick();

// Whether no game thread runs now (all of them wait).
bool KernelIdle();
std::uint64_t KernelIdleNs();
// A monotonic clock in nanoseconds, cheap to read (the Switch's CPU counter, no system call).
std::uint64_t NowNs();
// The id of the game thread that is running.
std::int32_t CurrentThreadId();
// The EE's float rules on the calling thread: rounding toward zero, denormals flushed.
void SetGameFloatMode();
// The pad's buttons the game last read, as its word holds them, set when pressed (port/platform/pad.cpp)
extern std::atomic<std::uint16_t> g_padHeld;
// The kernel model (port/platform/kernel.cpp): the calling host thread becomes the game's main
// thread, running; and an interrupt cause's handler is called (the clock: 2, vertical blank).
void KernelStartMainThread();
void RaiseInterrupt(int cause);
// A game thread's own wait loop gives way to a thread of higher priority made ready meanwhile.
void Spin();
// The kernel's threads and handlers, printed, for a diagnosis.
void KernelDump();

}  // namespace port

#endif
