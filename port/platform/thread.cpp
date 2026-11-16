// SPDX-License-Identifier: AGPL-3.0-or-later
// Threads on stacks in game memory (port/thread.h).
#include "port/thread.h"

#include <cstdint>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace port {

#if defined(__SWITCH__)

}  // namespace port

// Calls fn(arg) with the stack pointer at top, then returns on the caller's stack (x29, which
// the callee preserves, holds it meanwhile).
extern "C" void* port_run_on_stack(void* top, void* (*fn)(void*), void* arg);
asm(R"(
    .text
    .global port_run_on_stack
    .type port_run_on_stack, %function
    .p2align 2
port_run_on_stack:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov sp, x0
    mov x0, x2
    blr x1
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
    .size port_run_on_stack, . - port_run_on_stack
)");

namespace port {
namespace {

struct Start {
    void* top;
    void* (*entry)(void*);
    void* arg;
};

void* OnGameStack(void* p)
{
    Start s = *static_cast<Start*>(p);
    delete static_cast<Start*>(p);
    return port_run_on_stack(s.top, s.entry, s.arg);
}

}  // namespace

int StartThread(pthread_t* thread, void* stack, std::size_t size, void* (*entry)(void*), void* arg,
                bool detached)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);   // only the start runs on it
    if (detached)
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    // Room above the first frame, as a start-up frame would leave: game code writes into its
    // caller's frame (the argument area above its own, as on MIPS), and from the thread's first
    // frame that reached past the stack's end, into the kernel's globals the linker put there.
    constexpr std::uintptr_t kAbove = 0x400;
    auto top = reinterpret_cast<void*>((reinterpret_cast<std::uintptr_t>(stack) + size - kAbove) & ~std::uintptr_t(15));
    Start* s = new Start{top, entry, arg};
    int r = pthread_create(thread, &attr, OnGameStack, s);
    if (r != 0)
        delete s;
    pthread_attr_destroy(&attr);
    return r;
}

void PlaceThread(ThreadRole role)
{
    int core = role == ThreadRole::Game ? 0 : role == ThreadRole::Clock ? 1 : 2;
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    s32 prio = 0x2C;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (role != ThreadRole::Game && role != ThreadRole::Render)
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);   // above the game's (libnx gives 0x2C to main)
    else
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
}

#else

void PlaceThread(ThreadRole) {}

int StartThread(pthread_t* thread, void* stack, std::size_t size, void* (*entry)(void*), void* arg,
                bool detached)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int r = pthread_attr_setstack(&attr, stack, size);
    if (r == 0 && detached)
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (r == 0)
        r = pthread_create(thread, &attr, entry, arg);
    pthread_attr_destroy(&attr);
    return r;
}

#endif

}  // namespace port
