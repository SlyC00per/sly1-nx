// SPDX-License-Identifier: AGPL-3.0-or-later
// The EE kernel's threads, semaphores and interrupt handlers, as the game uses them (sly1-decomp
// src/lib/system_calls.cpp's calls; their roles in config/names/lib), on host threads.
//
// The PS2 runs one thread at a time: the ready thread of the highest priority (the lowest
// number), until it waits or a higher one becomes ready. The game depends on that (its render
// thread and main thread hand frames over through semaphores), so the port keeps it: each game
// thread is a host thread, but only the one the kernel has chosen runs; the others wait on their
// own condition until it hands over at a kernel call (a semaphore wait or signal, a thread
// started, ended or given another priority). Interrupt handlers are called by cause from the
// port's clock and devices (port::RaiseInterrupt), outside any game thread, as the PS2 calls
// them between a thread's instructions.
//
// A game thread runs on a stack of the port's in game memory (port/ptr32.h: the game links
// stack frames into its lists), not on the stack the game gives it, which is sized for the PS2.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <pthread.h>

#include "port/thread.h"

#include "common.h"
#include "thread_params.h"
#include "semaphore_params.h"
#include "port/ptr32.h"
#include "port/system.h"

namespace port {

std::atomic<bool> g_preemptPending{false};

// Interrupts raised by the clock while a game thread runs: serviced by that thread at its next
// preemption point or kernel call, with it stopped there, as the PS2 runs a handler between the
// interrupted thread's instructions, never beside them.
std::atomic<unsigned> g_pendingCauses{0};
std::mutex g_serviceLock;
std::condition_variable g_serviced;
void ServiceInterrupts();

namespace {

constexpr int kThreads = 32;
constexpr int kSemaphores = 256;
constexpr std::size_t kStackSize = 1 << 20;
constexpr int kIdle = 0;

enum State { Free, Dormant, Ready, Waiting, Ended };

struct Thread {
    State state = Free;
    int priority = 64;
    std::condition_variable cv;
    void (*entry)(void*) = nullptr;
    void* arg = nullptr;
    int stack = -1;
    int waitSema = 0;
};

struct Sema {
    bool used = false;
    int count = 0;
    int max = 0;
};

std::mutex g_kernel;
Thread g_threads[kThreads];
Sema g_semas[kSemaphores];
int g_current = kIdle;        // the game thread that runs, or none

alignas(16384) unsigned char g_stacks[8][kStackSize];   // page-aligned (port/thread.h)
bool g_stackUsed[8];

void (*g_handlers[16])(void);
bool g_enabled[16];

thread_local int t_self = kIdle;

// The ready thread of the highest priority, or idle.
int Pick()
{
    int best = kIdle;
    for (int i = 1; i < kThreads; i++)
        if (g_threads[i].state == Ready && (best == kIdle || g_threads[i].priority < g_threads[best].priority))
            best = i;
    return best;
}

// The calling game thread gives the processor to the best ready thread and waits until the
// kernel chooses it again (or returns at once when it is the best itself).
void Switch(std::unique_lock<std::mutex>& lk)
{
    int self = t_self;
    int next = Pick();
    if (next == self)
        return;
    g_current = next;
    if (next != kIdle)
        g_threads[next].cv.notify_one();
    g_threads[self].cv.wait(lk, [self] { return g_current == self; });
}

// A thread becomes ready; when it outranks the running one (the caller), the caller yields.
void MakeReady(std::unique_lock<std::mutex>& lk, int id, bool fromGameThread)
{
    g_threads[id].state = Ready;
    if (g_current == kIdle) {
        g_current = id;
        g_threads[id].cv.notify_one();
    } else if (g_threads[id].priority < g_threads[g_current].priority) {
        if (fromGameThread)
            Switch(lk);
        else
            g_preemptPending.store(true, std::memory_order_relaxed);   // at the next preemption point
    }
}

// Whether a ready thread outranks the running one: what the preemption points look at.
void UpdatePreemptPending()
{
    int best = Pick();
    bool pending = g_pendingCauses.load() != 0 || (g_current != kIdle && best != kIdle && best != g_current &&
                   g_threads[best].priority < g_threads[g_current].priority);
    g_preemptPending.store(pending, std::memory_order_relaxed);
}

void* HostEntry(void* p)
{
    int id = (int)(std::intptr_t)p;
    t_self = id;
    PlaceThread(ThreadRole::Game);
    SetGameFloatMode();
    {
        std::unique_lock<std::mutex> lk(g_kernel);
        g_threads[id].cv.wait(lk, [id] { return g_current == id; });
    }
    g_threads[id].entry(g_threads[id].arg);
    std::unique_lock<std::mutex> lk(g_kernel);
    g_threads[id].state = Ended;
    int next = Pick();
    g_current = next;
    if (next != kIdle)
        g_threads[next].cv.notify_one();
    return nullptr;
}

}  // namespace

// The game's first thread (retail's main thread): the calling host thread, running.
void KernelStartMainThread()
{
    std::lock_guard<std::mutex> lk(g_kernel);
    g_threads[1].state = Ready;
    g_threads[1].priority = 1;
    g_current = 1;
    t_self = 1;
}

std::int32_t CurrentThreadId() { return t_self; }

// The kernel's threads, for a diagnosis when the port stops: state, priority, what each waits on.
void KernelDump()
{
    static const char* kStates[] = {"free", "dormant", "ready", "waiting", "ended"};
    for (int i = 1; i < kThreads; i++) {
        if (g_threads[i].state == Free)
            continue;
        std::fprintf(stderr, "kernel: thread %d %s, priority %d%s", i, kStates[g_threads[i].state],
                     g_threads[i].priority, i == g_current ? ", running" : "");
        if (g_threads[i].state == Waiting)
            std::fprintf(stderr, ", on semaphore %d (count %d)", g_threads[i].waitSema,
                         g_semas[g_threads[i].waitSema].count);
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "kernel: running %d; interrupt handlers:", g_current);
    for (int c = 0; c < 16; c++)
        if (g_handlers[c])
            std::fprintf(stderr, " %d%s", c, g_enabled[c] ? "" : " (disabled)");
    std::fprintf(stderr, "\n");
}

// Calls the handlers of an interrupt cause, from the port's clock or a device.
// A preemption point (port/retail_data.h): on the PS2 an interrupt that readies a thread of higher
// priority preempts the running one where it is; here the running thread gives way at its next
// loop test or kernel call. The interrupt handlers (on the clock's host thread) never do.
void PreemptSlow()
{
    ServiceInterrupts();
    std::unique_lock<std::mutex> lk(g_kernel);
    int self = t_self;
    if (self != kIdle && g_current == self) {
        int best = Pick();
        if (best != self && best != kIdle && g_threads[best].priority < g_threads[self].priority)
            Switch(lk);
    }
    UpdatePreemptPending();
}

// A game thread's own wait loop (an empty-bodied while, do or for(;;)): a preemption point, then
// a moment for the host's other threads.
void Spin()
{
    Preempt();
    std::this_thread::yield();
}

namespace {

// One handler at a time, as on the PS2; a handler that starts a transfer may raise another
// inside it.
void Dispatch(int cause)
{
    static std::recursive_mutex interrupts;
    std::lock_guard<std::recursive_mutex> lk(interrupts);
    if (cause >= 0 && cause < 16 && g_enabled[cause] && g_handlers[cause])
        g_handlers[cause]();
}

}  // namespace

// The running game thread runs the interrupts the clock left pending (at a preemption point or a
// kernel call), stopped there while they run.
void ServiceInterrupts()
{
    if (t_self == kIdle || g_pendingCauses.load(std::memory_order_relaxed) == 0)
        return;
    unsigned causes = g_pendingCauses.exchange(0);
    for (int c = 0; c < 16; c++)
        if (causes & (1u << c))
            Dispatch(c);
    std::lock_guard<std::mutex> lk(g_serviceLock);
    g_serviced.notify_all();
}

// A device on a game thread (a DMA transfer's VIF or GS interrupt) runs the handler at once, the
// thread stopped in it. The clock, a host thread of its own, leaves it pending for the running
// game thread and waits until it has run; with no game thread running it runs it itself.
void RaiseInterrupt(int cause)
{
    if (t_self != kIdle) {
        Dispatch(cause);
        return;
    }
    g_pendingCauses.fetch_or(1u << cause);
    g_preemptPending.store(true);
    std::unique_lock<std::mutex> lk(g_serviceLock);
    for (;;) {
        if ((g_pendingCauses.load() & (1u << cause)) == 0)
            return;
        bool running;
        {
            std::lock_guard<std::mutex> k(g_kernel);
            running = g_current != kIdle;
        }
        if (!running) {
            // every game thread waits: nothing to interrupt
            unsigned mine = g_pendingCauses.fetch_and(~(1u << cause));
            if (mine & (1u << cause)) {
                lk.unlock();
                Dispatch(cause);
            }
            return;
        }
        g_serviced.wait_for(lk, std::chrono::milliseconds(1));
    }
}

}  // namespace port

using namespace port;

// --- the kernel's calls (C++ linkage, as the game declares them)

s32 MakeThread(ThreadParams* p)
{
    std::lock_guard<std::mutex> lk(g_kernel);
    for (int i = 2; i < kThreads; i++) {
        if (g_threads[i].state == Free || g_threads[i].state == Ended) {
            g_threads[i].state = Dormant;
            g_threads[i].priority = p->priority;
            g_threads[i].entry = (void (*)(void*))(void (*)())p->entry;
            return i;
        }
    }
    std::fprintf(stderr, "port: out of threads\n");
    return -1;
}

void RunThread(s32 id, void* arg)
{
    std::unique_lock<std::mutex> lk(g_kernel);
    if (id <= 1 || id >= kThreads || g_threads[id].state != Dormant)
        return;
    int s = -1;
    for (int i = 0; i < 8; i++)
        if (!g_stackUsed[i]) {
            s = i;
            break;
        }
    if (s < 0) {
        std::fprintf(stderr, "port: out of thread stacks\n");
        std::abort();
    }
    g_stackUsed[s] = true;
    g_threads[id].stack = s;
    g_threads[id].arg = arg;
    pthread_t h;
    if (StartThread(&h, g_stacks[s], kStackSize, HostEntry, (void*)(std::intptr_t)id, true) != 0) {
        std::fprintf(stderr, "port: a game thread on its stack in game memory could not start\n");
        std::abort();
    }
    MakeReady(lk, id, true);
}

s32 EndOwnThread()
{
    {
        std::lock_guard<std::mutex> lk(g_kernel);
        int self = t_self;
        g_threads[self].state = Ended;
        if (g_threads[self].stack >= 0)
            g_stackUsed[g_threads[self].stack] = false;
        int next = Pick();
        g_current = next;
        if (next != kIdle)
            g_threads[next].cv.notify_one();
    }
    pthread_exit(nullptr);
}

void SetThreadPriority(s32 id, s32 priority)
{
    std::unique_lock<std::mutex> lk(g_kernel);
    if (id < 1 || id >= kThreads)
        return;
    g_threads[id].priority = priority;
    if (t_self != kIdle)
        Switch(lk);
}

void AddInterruptHandler(s32 cause, void (*fn)(), s32)
{
    if (cause >= 0 && cause < 16)
        g_handlers[cause] = fn;
}

s32 EnableInterruptCause(s32 cause)
{
    if (cause >= 0 && cause < 16)
        g_enabled[cause] = true;
    return 1;
}

extern "C" {

s32 GetCurrentThreadId(void) { return t_self; }

s32 NewSemaphore(void* self)
{
    const SemaphoreParams* p = (const SemaphoreParams*)self;
    std::lock_guard<std::mutex> lk(g_kernel);
    for (int i = 1; i < kSemaphores; i++) {
        if (!g_semas[i].used) {
            g_semas[i].used = true;
            g_semas[i].count = p->initCount;
            g_semas[i].max = p->maxCount;
            return i;
        }
    }
    std::fprintf(stderr, "port: out of semaphores\n");
    return -1;
}

// Signals a semaphore: hands it to the waiting thread of the highest priority, or counts it.
static s32 Signal(s32 id, bool fromGameThread)
{
    std::unique_lock<std::mutex> lk(g_kernel);
    if (id < 1 || id >= kSemaphores || !g_semas[id].used)
        return -1;
    int waiter = kIdle;
    for (int i = 1; i < kThreads; i++)
        if (g_threads[i].state == Waiting && g_threads[i].waitSema == id &&
            (waiter == kIdle || g_threads[i].priority < g_threads[waiter].priority))
            waiter = i;
    if (waiter != kIdle) {
        g_threads[waiter].waitSema = 0;
        MakeReady(lk, waiter, fromGameThread);
    } else if (g_semas[id].max <= 0 || g_semas[id].count < g_semas[id].max) {
        g_semas[id].count++;
    }
    return id;
}

s32 PostSemaphore(void* a)
{
    ServiceInterrupts();
    return Signal((s32)port::WordArg(a), t_self != kIdle);
}
void PostSemaphoreFromInterrupt(s32 sema) { Signal(sema, false); }

s32 AwaitSemaphore(void* a)
{
    ServiceInterrupts();
    s32 id = (s32)port::WordArg(a);
    std::unique_lock<std::mutex> lk(g_kernel);
    if (id < 1 || id >= kSemaphores || !g_semas[id].used)
        return -1;
    if (g_semas[id].count > 0) {
        g_semas[id].count--;
        return id;
    }
    int self = t_self;
    g_threads[self].state = Waiting;
    g_threads[self].waitSema = id;
    Switch(lk);   // back when a signal has handed the semaphore over
    return id;
}

}  // extern "C"
