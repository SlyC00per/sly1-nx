// SPDX-License-Identifier: AGPL-3.0-or-later
// The port's include/system.h (tools/gen.py puts it in place of the decompilation's): the same
// helpers, on the port's platform layer instead of the EE's system instructions.
#ifndef SYSTEM_H
#define SYSTEM_H

#include <atomic>

#include "common.h"
#include "port/system.h"

// sync: earlier memory operations complete first.
static inline void Sync(void) { std::atomic_thread_fence(std::memory_order_seq_cst); }
// sync; ei: the end of an interrupt handler. The port's handlers are ordinary callbacks.
static inline void SyncAndEnableInterrupts(void) { std::atomic_thread_fence(std::memory_order_seq_cst); }
// The COP0 Status register: interrupts enabled (EIE, bit 16), as the game runs.
static inline u32 ReadCop0Status(void) { return port::Cop0Status(); }
// The COP0 Count register: the EE's cycle counter (294.912 MHz), from the port's clock.
static inline u64 ReadCop0Count(void) { return port::CycleCount(); }
// The TLB's Wired register: the port has no TLB.
static inline void SetTlbWired(u32) {}
// The current thread's id, as the kernel gives it to an interrupt handler.
static inline s32 CurrentThreadIdFromInterrupt(void) { return port::CurrentThreadId(); }

#endif
