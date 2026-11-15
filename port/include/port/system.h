// SPDX-License-Identifier: AGPL-3.0-or-later
// The platform layer's answers to the EE's system state (port/ps2/system.h uses them).
#ifndef PORT_SYSTEM_H
#define PORT_SYSTEM_H

#include <cstdint>

namespace port {

// The COP0 Status register as the game sees it: interrupts enabled (EIE, bit 16).
std::uint32_t Cop0Status();
// The EE's cycle counter: 294,912,000 counts a second, from the port's clock (in a deterministic
// run, from the frame count).
std::uint64_t CycleCount();
// The id of the game thread that is running.
std::int32_t CurrentThreadId();
// The EE's float rules on the calling thread: rounding toward zero, denormals flushed.
void SetGameFloatMode();
// The kernel model (port/platform/kernel.cpp): the calling host thread becomes the game's main
// thread, running; and an interrupt cause's handler is called (the clock: 2, vertical blank).
void KernelStartMainThread();
void RaiseInterrupt(int cause);
// The kernel's threads and handlers, printed, for a diagnosis.
void KernelDump();

}  // namespace port

#endif
