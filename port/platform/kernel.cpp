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

#include <csetjmp>
#include <cstring>
#include <pthread.h>

#include "port/thread.h"

#include "common.h"
#include "thread_params.h"
#include "semaphore_params.h"
#include "port/ptr32.h"
#include "port/system.h"
#include "port/dmac.h"

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
std::uint64_t g_idleSince, g_idleNs;   // how long no game thread has run, for the report

// The running game thread changes (under g_kernel): the time with none running is counted.
void SetCurrent(int next)
{
    if (g_current == kIdle && next != kIdle && g_idleSince)
        g_idleNs += NowNs() - g_idleSince;
    if (next == kIdle && g_current != kIdle)
        g_idleSince = NowNs();
    g_current = next;
}

// Each stack has a guard above it, filled with a pattern: game code has been seen writing above
// its thread's first frame, which on the Switch reached the kernel's globals the linker put after
// the stacks. A changed guard is reported (CheckStackGuards) rather than left to corrupt them.
constexpr std::size_t kStackGuard = 64 * 1024;
constexpr unsigned char kGuardByte = 0xA5;
#ifdef PORT_SENTINEL
}  // namespace
// Zero, in .bss: declared just before the stacks, which GCC then places it right after (it lays
// out a file's .bss in reverse order), the place a stray store hit on the Switch.
volatile std::uint32_t g_stackEndSentinel[4];
namespace {
#endif
alignas(16384) unsigned char g_stacks[8][kStackSize + kStackGuard];   // page-aligned (port/thread.h)
bool g_stackUsed[8];
bool g_guardFilled[8];

void FillStackGuard(int s)
{
    std::memset(g_stacks[s] + kStackSize, kGuardByte, kStackGuard);
    g_guardFilled[s] = true;
}

// Reports (once per change) bytes written into a stack's guard: the stack, the offsets above its
// top, and the first words there.
void CheckStackGuard(int s, const char* when)
{
    if (!g_guardFilled[s])
        return;
    const unsigned char* g = g_stacks[s] + kStackSize;
    std::size_t first = kStackGuard, last = 0;
    for (std::size_t i = 0; i < kStackGuard; i++)
        if (g[i] != kGuardByte) {
            if (first == kStackGuard)
                first = i;
            last = i;
        }
    if (first == kStackGuard)
        return;
    std::fprintf(stderr, "kernel: stack %d (game 0x%08X) written above its top at +0x%zX..+0x%zX (%s):", s,
                 GameAddr(g), first, last, when);
    for (std::size_t i = first & ~std::size_t(3); i <= last && i < first + 32; i += 4)
        std::fprintf(stderr, " %08X", *reinterpret_cast<const std::uint32_t*>(g + i));
    std::fprintf(stderr, "\n");
    FillStackGuard(s);
}

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
    SetCurrent(next);
    if (next != kIdle)
        g_threads[next].cv.notify_one();
    g_threads[self].cv.wait(lk, [self] { return g_current == self; });
}

// A thread becomes ready; when it outranks the running one (the caller), the caller yields.
void MakeReady(std::unique_lock<std::mutex>& lk, int id, bool fromGameThread)
{
    g_threads[id].state = Ready;
    if (g_current == kIdle) {
        SetCurrent(id);
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

// A game thread from its start to its return, on its slot's host thread.
void RunGameThread(int id)
{
    t_self = id;
    {
        std::unique_lock<std::mutex> lk(g_kernel);
        g_threads[id].cv.wait(lk, [id] { return g_current == id; });
    }
    g_threads[id].entry(g_threads[id].arg);
    std::unique_lock<std::mutex> lk(g_kernel);
    g_threads[id].state = Ended;
    if (g_threads[id].stack >= 0) {   // returned from its entry: its stack is free again
        CheckStackGuard(g_threads[id].stack, "the thread returned");
        g_stackUsed[g_threads[id].stack] = false;
        g_threads[id].stack = -1;
    }
    int next = Pick();
    SetCurrent(next);
    if (next != kIdle)
        g_threads[next].cv.notify_one();
}

// Each stack has a host thread of its own, started with the first game thread given that stack
// and kept: it runs the game threads given its stack one after another. (A host thread for each
// game thread ran out on the Switch, which does not take back a detached thread's resources
// fully; the game starts and ends threads at every load.) A thread that ends itself
// (EndOwnThread, deep in game code) comes back to its slot's loop with longjmp, as pthread_exit
// left it before: the game's frames have nothing to destroy.
struct StackSlot {
    std::condition_variable cv;
    int id = -1;          // the game thread to run, or -1
    bool started = false;
    std::jmp_buf end;
};
StackSlot g_slots[8];

void* SlotWorker(void* p)
{
    int s = (int)(std::intptr_t)p;
    PlaceThread(ThreadRole::Game);
    SetGameFloatMode();
    for (;;) {
        int id;
        {
            std::unique_lock<std::mutex> lk(g_kernel);
            g_slots[s].cv.wait(lk, [s] { return g_slots[s].id >= 0; });
            id = g_slots[s].id;
        }
        if (setjmp(g_slots[s].end) == 0)
            RunGameThread(id);
        std::lock_guard<std::mutex> lk(g_kernel);
        if (g_slots[s].id == id)   // not given the next thread already
            g_slots[s].id = -1;
    }
}

}  // namespace

// The game's first thread (retail's main thread): the calling host thread, running.
void KernelStartMainThread()
{
    std::lock_guard<std::mutex> lk(g_kernel);
    g_threads[1].state = Ready;
    g_threads[1].priority = 1;
    SetCurrent(1);
    t_self = 1;
}

std::int32_t CurrentThreadId() { return t_self; }

// The kernel's threads, for a diagnosis when the port stops: state, priority, what each waits on.
// How long no game thread has run so far (while none runs now, up to the last time one stopped).
std::uint64_t KernelIdleNs() { return g_idleNs; }

bool KernelIdle()
{
    std::lock_guard<std::mutex> lk(g_kernel);
    return g_current == kIdle && g_pendingCauses.load() == 0;
}

void KernelDump()
{
    std::fprintf(stderr, "kernel: no game thread ran for %.2f s\n", g_idleNs / 1e9);
    for (int s = 0; s < 8; s++)
        CheckStackGuard(s, "the kernel's report");
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
float WideFactor()
{
    static const float factor = std::getenv("SLY1_WIDE") && std::getenv("SLY1_WIDE")[0] == '0' ? 1.0f : 4.0f / 3.0f;
    return factor;
}

namespace {
float g_wideAnchor = -1.0f;   // the x the element being drawn is narrowed about, or none
}
void Wide2D(float middle)
{
    if (middle < 0.0f || WideFactor() == 1.0f)
        g_wideAnchor = -1.0f;
    else
        g_wideAnchor = middle < 640.0f / 3 ? 0.0f : middle > 640.0f * 2 / 3 ? 640.0f : 320.0f;
}
int WideX(int x)
{
    if (g_wideAnchor < 0.0f)
        return x;
    const float at = (float)x / 16.0f - 1728.0f;
    if (at <= 0.5f || at >= 639.5f)
        return x;
    return (int)((g_wideAnchor + (at - g_wideAnchor) / WideFactor() + 1728.0f) * 16.0f);
}

// A call for the game's own thread to make (the host's diagnosis: placing the player), taken at
// the next point where a game thread lets others in.
std::atomic<void (*)()> g_gameThreadCall{nullptr};
void NoteWorldLoad(int arrivalA, int arrivalB, int flags)
{
    std::fprintf(stderr, "world: a level load, arrival keys %d %d, flags 0x%X\n", arrivalA, arrivalB, (unsigned)flags);
}

void NoteFade(long from, int kind, int value, int count, int mode)
{
    if (from == -1)
        std::fprintf(stderr, "world: the completion fade, kind %d, completion mask 0x%X\n", kind, (unsigned)value);
    else
        std::fprintf(stderr, "world: a fade asked for from StartScreenFade%+ld: kind %d, value %d, style %d, the fade's mode %d\n", from, kind, value, count, mode);
}

void CallOnGameThread(void (*fn)()) { g_gameThreadCall.store(fn, std::memory_order_release); }

void PreemptSlow()
{
    if (g_gameThreadCall.load(std::memory_order_relaxed))
        if (void (*fn)() = g_gameThreadCall.exchange(nullptr, std::memory_order_acq_rel))
            fn();
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
    FillStackGuard(s);
    g_threads[id].stack = s;
    g_threads[id].arg = arg;
    g_slots[s].id = id;
    if (!g_slots[s].started) {
        pthread_t h;
        if (StartThread(&h, g_stacks[s], kStackSize, SlotWorker, (void*)(std::intptr_t)s, true) != 0) {
            std::fprintf(stderr, "port: a game thread on its stack in game memory could not start\n");
            std::abort();
        }
        g_slots[s].started = true;
    } else {
        g_slots[s].cv.notify_one();
    }
    MakeReady(lk, id, true);
}

s32 EndOwnThread()
{
    int slot = -1;
    {
        std::lock_guard<std::mutex> lk(g_kernel);
        int self = t_self;
        g_threads[self].state = Ended;
        if (g_threads[self].stack >= 0) {
            slot = g_threads[self].stack;
            CheckStackGuard(slot, "the thread ended");
            g_stackUsed[slot] = false;
            g_threads[self].stack = -1;
        }
        int next = Pick();
        SetCurrent(next);
        if (next != kIdle)
            g_threads[next].cv.notify_one();
    }
    if (slot >= 0)
        std::longjmp(g_slots[slot].end, 1);   // back to the slot's loop (SlotWorker)
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

// SLY1_TIMELINE (port/platform/dmac.cpp): a semaphore posted or awaited, by which thread.
static void TimelineSemaphore(const char* what, s32 id)
{
    static const bool on = std::getenv("SLY1_TIMELINE") != nullptr;
    if (!on || id < 2 || id > 5)   // the vertical blank's, the GS's FINISH, a frame ready, a frame done
        return;
    char line[64];
    std::snprintf(line, sizeof line, "thread %d %s semaphore %d", t_self, what, (int)id);
    port::TimelineEvent(line);
}

s32 PostSemaphore(void* a)
{
    ServiceInterrupts();
    TimelineSemaphore("posts", (s32)port::WordArg(a));
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
    lk.unlock();
    TimelineSemaphore("waits on", id);
    lk.lock();
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

#ifdef PORT_SENTINEL
namespace port {

void SentinelTripped()
{
    std::fprintf(stderr, "kernel: the word after the stacks (game 0x%08X) changed to %08X %08X %08X %08X\n",
                 GameAddr((const void*)g_stackEndSentinel), g_stackEndSentinel[0], g_stackEndSentinel[1],
                 g_stackEndSentinel[2], g_stackEndSentinel[3]);
    {   // the packet writers (g_gsPacketWriter 0x262280, g_vifPacketWriter 0x2622A8: end, begin, cur
        // at +0, +4, +0xC) and the frame being built (0x2622D8)
        auto w = [](std::uint32_t a) { return *(const std::uint32_t*)Native(a); };
        std::fprintf(stderr, "kernel: GS writer end %08X begin %08X cur %08X; VIF writer end %08X begin %08X cur %08X; "
                     "build frame %08X; thread %d on stack %d\n", w(0x262280), w(0x262284), w(0x26228C), w(0x2622A8),
                     w(0x2622AC), w(0x2622B4), w(0x2622D8), t_self, t_self > 0 ? g_threads[t_self].stack : -1);
    }
    for (int i = 1; i < kThreads; i++)
        if (g_threads[i].state != Free)
            std::fprintf(stderr, "kernel: thread %d state %d priority %d stack %d entry %p\n", i, (int)g_threads[i].state,
                         g_threads[i].priority, g_threads[i].stack,
                         (void*)(reinterpret_cast<std::uintptr_t>(g_threads[i].entry) - ImageBase()));
    auto* fp = static_cast<std::uintptr_t*>(__builtin_frame_address(0));
    for (int i = 0; i < 16 && fp && reinterpret_cast<std::uintptr_t>(fp) % 8 == 0; i++) {
        std::uintptr_t lr = fp[1];
        if (lr - ImageBase() >= ImageSize())
            break;
        std::fprintf(stderr, "kernel:   from image +0x%llX\n", (unsigned long long)(lr - ImageBase()));
        fp = reinterpret_cast<std::uintptr_t*>(fp[0]);
    }
    std::fflush(stderr);
    std::fprintf(stderr, "sly1: done, status 5\n");
    std::fflush(stderr);
    std::_Exit(5);
}
}  // namespace port
#endif
