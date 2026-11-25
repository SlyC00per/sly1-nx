// SPDX-License-Identifier: AGPL-3.0-or-later
// Threads on stacks in game memory (port/thread.h).
#include "port/thread.h"
#include "port/crashlog.h"

#include <cstdint>
#include <algorithm>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "port/gs.h"
#include "port/ptr32.h"

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

namespace {
Handle g_sampled[3];   // the render thread's and the GS thread's; the game's main thread in its window (SampleWindow)
std::vector<std::uint32_t> g_samples[3];
std::map<std::vector<std::uint32_t>, std::uint32_t> g_mainStacks;   // the main thread's: what its stack holds of code addresses, counted
std::map<std::vector<std::uint32_t>, std::uint32_t> g_chains[2];   // the render and GS threads': the callers, by the frame records
std::atomic<std::uint64_t> g_windowFrom{0};   // (armGetSystemTick's value when the window opened; 0 shut)
std::atomic<bool> g_sampling;
Thread g_sampler;

}  // namespace
void SamplerReport();
namespace {
void Sampler(void*)
{
    svcSleepThread(12000000000ll);
    const std::uint64_t base = (std::uint64_t)ImageBase();
    // SLY1_SAMPLE_FRAMES=A,B: only while the frame shown is one of A to B
    std::uint64_t from = 0, to = ~0ull;
    if (const char* f = std::getenv("SLY1_SAMPLE_FRAMES")) {
        unsigned long long a = 0, b = 0;
        if (std::sscanf(f, "%llu,%llu", &a, &b) == 2) {
            from = a;
            to = b;
        }
    }
    // SLY1_SAMPLE=2: the render and GS threads only while fewer than 48 frames a second are shown
    // (told each quarter of a second), and the report after 40 s of them
    static const bool slowOnly = std::getenv("SLY1_SAMPLE")[0] == '2';
    bool slow = false;
    std::uint64_t markTick = armGetSystemTick(), markShown = gs::FramesShown();
    while (g_sampling.load()) {
        svcSleepThread(1000000);
        const std::uint64_t shown = gs::FramesShown();
        if (armTicksToNs(armGetSystemTick() - markTick) >= 250000000ull) {
            slow = shown - markShown < 12;
            markTick = armGetSystemTick();
            markShown = shown;
        }
        if (shown < from || shown > to)
            continue;
        for (int k = 0; k < 3; k++) {
            if (!g_sampled[k] || g_samples[k].size() >= 120000)
                continue;
            if (k < 2 && slowOnly && !slow)
                continue;
            if (k == 2) {
                const std::uint64_t from = g_windowFrom.load();
                if (!from || armTicksToNs(armGetSystemTick() - from) < 2000000)
                    continue;
            }
            ThreadContext ctx;
            if (R_FAILED(svcSetThreadActivity(g_sampled[k], ThreadActivity_Paused)))
                continue;
            const bool got = R_SUCCEEDED(svcGetThreadContext3(&ctx, g_sampled[k]));
            // Its callers by the frame records (x29: the caller's record and the return address), while
            // they lie in order above the stack pointer. Into an array: nothing is allocated while the
            // thread is stopped (it may be stopped inside the allocator, holding its lock).
            std::uint32_t chain[11];
            int links = 0;
            if (got && k < 2) {
                chain[links++] = (std::uint32_t)std::min<std::uint64_t>(ctx.pc.x - base, 0xFFFFFFFFu);
                chain[links++] = (std::uint32_t)std::min<std::uint64_t>(ctx.cpu_gprs[30].x - base, 0xFFFFFFFFu);
                std::uint64_t fp = ctx.fp, low = ctx.sp;
                for (int depth = 0; depth < 9 && fp >= low && fp < ctx.sp + 0x40000 && (fp & 15) == 0; depth++) {
                    const std::uint64_t* record = reinterpret_cast<const std::uint64_t*>(fp);
                    chain[links++] = (std::uint32_t)std::min<std::uint64_t>(record[1] - base, 0xFFFFFFFFu);
                    low = fp + 16;
                    fp = record[0];
                }
            }
            if (got && k == 2) {
                // where it was called from: the link register, and the first words of its stack that
                // point into the program's code (return addresses, with some stale ones among them)
                std::vector<std::uint32_t> stack;
                stack.push_back((std::uint32_t)(ctx.cpu_gprs[30].x - base));
                const std::uint64_t* sp = reinterpret_cast<const std::uint64_t*>(ctx.sp & ~7ull);
                for (int w = 0; w < 600 && stack.size() < 14; w++)
                    if (sp[w] > base && sp[w] - base < 0x500000 && ((sp[w] - base) & 3) == 0)
                        stack.push_back((std::uint32_t)(sp[w] - base));
                g_mainStacks[stack]++;
            }
            svcSetThreadActivity(g_sampled[k], ThreadActivity_Runnable);
            if (links && g_chains[k].size() < 30000)
                g_chains[k][std::vector<std::uint32_t>(chain, chain + links)]++;
            if (got)
                g_samples[k].push_back((std::uint32_t)std::min<std::uint64_t>(ctx.pc.x - base, 0xFFFFFFFFu));
        }
        // the report as soon as there are 60,000 samples of each (a minute): a run that is stopped
        // from outside, or left by the player, never reaches the one at the program's end
        if (g_samples[0].size() >= (slowOnly ? 40000u : 60000u) && g_samples[1].size() >= (slowOnly ? 40000u : 60000u)) {
            SamplerReport();
            return;
        }
    }
}
}  // namespace

void StartSampler()
{
    if (!std::getenv("SLY1_SAMPLE"))
        return;
    for (auto& v : g_samples)
        v.reserve(120000);
    g_sampling.store(true);
    if (R_SUCCEEDED(threadCreate(&g_sampler, Sampler, nullptr, nullptr, 0x8000, 0x2A, 0)))
        threadStart(&g_sampler);
}

void SamplerReport()
{
    if (!g_sampling.exchange(false))
        return;
    svcSleepThread(5000000);
    static const char* const kName[3] = {"render", "gs", "main"};
    for (int k = 0; k < 3; k++) {
        std::map<std::uint32_t, std::uint32_t> count;   // by 64 bytes of code
        for (std::uint32_t at : g_samples[k])
            count[at >> 6]++;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> top;
        for (auto& c : count)
            top.push_back({c.second, c.first});
        std::sort(top.rbegin(), top.rend());
        std::fprintf(stderr, "sample: %s thread, %zu samples\n", kName[k], g_samples[k].size());
        for (std::size_t i = 0; i < top.size() && i < 400; i++)
            std::fprintf(stderr, "sample: %s %08X %u\n", kName[k], top[i].second << 6, top[i].first);
    }
    for (int k = 0; k < 2; k++) {
        std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> chains;
        for (auto& s : g_chains[k])
            chains.push_back({s.second, s.first});
        std::sort(chains.rbegin(), chains.rend());
        for (std::size_t i = 0; i < chains.size() && i < 4000; i++) {
            std::fprintf(stderr, "samplechain: %s %u:", kName[k], chains[i].first);
            for (std::uint32_t at : chains[i].second)
                std::fprintf(stderr, " %08X", at);
            std::fprintf(stderr, "\n");
        }
    }
    std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> stacks;
    for (auto& s : g_mainStacks)
        stacks.push_back({s.second, s.first});
    std::sort(stacks.rbegin(), stacks.rend());
    for (std::size_t i = 0; i < stacks.size() && i < 12; i++) {
        std::fprintf(stderr, "samplestack: %u:", stacks[i].first);
        for (std::uint32_t at : stacks[i].second)
            std::fprintf(stderr, " %08X", at);
        std::fprintf(stderr, "\n");
    }
}

void SampleWindow(bool open)
{
    static const bool on = std::getenv("SLY1_SAMPLE") && std::getenv("SLY1_SAMPLE")[0] == '3';   // (3: the main thread's window too)
    if (!on || !g_sampling.load())
        return;
    if (open)
        g_sampled[2] = threadGetCurHandle();
    g_windowFrom.store(open ? armGetSystemTick() : 0);
}

// (diagnosis of the start-up fault: newlib's table of devices, whose entries are never null, found
// with a null one at the first read of the disc) Says where the table is first seen changed.
extern "C" const void* devoptab_list[];
void CheckDevTable(const char* where)
{
    static std::atomic<int> said{0};
    static const void* first[8];
    static std::atomic<bool> have{false};
    if (!have.exchange(true)) {
        for (int k = 0; k < 8; k++)
            first[k] = devoptab_list[k];
        std::fprintf(stderr, "devtable: at %s: %p %p %p %p %p %p %p %p\n", where, first[0], first[1], first[2], first[3], first[4], first[5], first[6], first[7]);
    }
    for (int k = 0; k < 8; k++)
        if (devoptab_list[k] != first[k] && said.fetch_add(1) < 12) {
            // (said through the system's debug output too: the table's device may be the one that prints)
            char line[160];
            std::snprintf(line, sizeof line, "devtable: at %s: entry %d is %p, was %p", where, k, devoptab_list[k], first[k]);
            svcOutputDebugString(line, __builtin_strlen(line));
            std::fprintf(stderr, "%s\n", line);
        }
}

// (diagnosis) newlib's RemoveDevice, which alone sets an entry of the table to null, wrapped at the
// link (-Wl,--wrap=RemoveDevice): who takes a device out, by the return addresses.
extern "C" int __real_RemoveDevice(const char* name);
extern "C" int __wrap_RemoveDevice(const char* name)
{
    char line[200];
    const std::uint64_t base = (std::uint64_t)ImageBase();
    int n = std::snprintf(line, sizeof line, "devtable: RemoveDevice(%s) called from", name ? name : "(null)");
    const std::uint64_t* fp = reinterpret_cast<const std::uint64_t*>(__builtin_frame_address(0));
    for (int depth = 0; depth < 8 && fp && n < (int)sizeof line - 20; depth++) {
        n += std::snprintf(line + n, sizeof line - (std::size_t)n, " +0x%llX", (unsigned long long)(fp[1] - base));
        const std::uint64_t* up = reinterpret_cast<const std::uint64_t*>(fp[0]);
        if (up <= fp)
            break;
        fp = up;
    }
    svcOutputDebugString(line, __builtin_strlen(line));
    std::fprintf(stderr, "%s\n", line);
    return __real_RemoveDevice(name);
}

// Every thread the program starts and has not closed (libnx's threadCreate and threadClose wrapped
// at the link, -Wl,--wrap=...: pthread_create's threads and StartThread's alike), for
// StopOtherThreads.
extern "C" Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack, size_t stackSize, int prio, int cpu);
extern "C" Result __real_threadClose(Thread* t);
extern "C" Result __real_svcWaitSynchronization(s32* index, const Handle* handles, s32 count, u64 timeout);
namespace {
constexpr int kThreadsMost = 256;
std::atomic<Thread*> g_threads[kThreadsMost];
std::uintptr_t g_threadEntry[kThreadsMost];   // each one's first function and argument, for naming one that does not end
std::uintptr_t g_threadArg[kThreadsMost];
// Its handle and its stack's two places, copied when it is made: the Thread itself may be gone by
// the end (a detached thread's is freed when it ends; read then, the end faulted, 2026-11-25).
Handle g_threadHandle[kThreadsMost];
void* g_threadMirror[kThreadsMost];
void* g_threadStack[kThreadsMost];

// Whether the thread of a slot has ended (a handle no longer one counts as ended).
bool Ended(int k)
{
    s32 index = 0;
    const Result rc = __real_svcWaitSynchronization(&index, &g_threadHandle[k], 1, 0);
    return rc != 0xEA01;   // (anything but "timed out": it ended, or its handle is closed)
}
}
extern "C" Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack, size_t stackSize, int prio, int cpu)
{
    const Result rc = __real_threadCreate(t, entry, arg, stack, stackSize, prio, cpu);
    if (R_FAILED(rc)) {   // (the tenth run or so in one process could not start a thread, 2026-11-25: why, said)
        std::fprintf(stderr, "sly1: a thread could not be made: answer 0x%X (module %u, description %u)\n", rc, R_MODULE(rc), R_DESCRIPTION(rc));
    }
    if (R_SUCCEEDED(rc))
        for (int k = 0; k < kThreadsMost; k++) {
            Thread* none = nullptr;
            if (g_threads[k].compare_exchange_strong(none, t)) {
                g_threadEntry[k] = (std::uintptr_t)entry;
                g_threadArg[k] = (std::uintptr_t)arg;
                g_threadHandle[k] = t->handle;
                g_threadMirror[k] = t->stack_mirror;
                g_threadStack[k] = t->stack_mem;
                break;
            }
        }
    return rc;
}
extern "C" Result __wrap_threadClose(Thread* t)
{
    for (auto& slot : g_threads) {
        Thread* it = t;
        if (slot.compare_exchange_strong(it, nullptr))
            break;
    }
    return __real_threadClose(t);
}

// Threads that end themselves (EndOtherThreads). A thread cannot be ended from outside, and one
// only stopped stays in the process: a dozen a run, and the tenth run through the receiver could
// not make its threads (2026-11-25). So the three calls a thread waits in are wrapped at the link
// (-Wl,--wrap=...): a sleep, a wait on a condition variable (every std::condition_variable and
// semaphore) and a wait on the system's objects. Once the end is asked for, a thread that comes
// to one of them, or wakes from one, ends there: long waits are cut into short ones, and the
// condition variables anyone has waited on are signalled. Never the first thread, which carries
// the end out. A thread ends holding whatever locks it held, but for the wait's own mutex, which
// it gives up; one that then waits for such a lock for ever is stopped as before.
extern "C" void __real_svcSleepThread(s64 nano);
extern "C" Result __real_svcWaitSynchronization(s32* index, const Handle* handles, s32 count, u64 timeout);
extern "C" Result __real_condvarWaitTimeout(CondVar* c, Mutex* m, u64 timeout);
namespace {
std::atomic<bool> g_threadsEnd;
// The condition variables threads are waiting on now, one slot a waiting thread. Only these are
// signalled at the end: the kernel writes to a condition variable it signals (0, when nobody
// waits on it any more), and one that is gone -- a function's local -- is by then something
// else's memory: signalling every one ever waited on zeroed half a pointer on the first
// thread's stack, which faulted in each run's end (2026-11-25).
constexpr int kWaitersMost = 64;
std::atomic<CondVar*> g_waitingOn[kWaitersMost];

inline bool EndHere()
{
    return g_threadsEnd.load(std::memory_order_relaxed) && threadGetCurHandle() != envGetMainThreadHandle();
}

// A slot taken for a wait on `c` (-1: none free, the wait is then not signalled at the end and
// its thread is stopped instead).
int NoteWait(CondVar* c)
{
    for (int k = 0; k < kWaitersMost; k++) {
        CondVar* none = nullptr;
        if (g_waitingOn[k].load(std::memory_order_relaxed) == nullptr && g_waitingOn[k].compare_exchange_strong(none, c))
            return k;
    }
    return -1;
}
constexpr s64 kSliceNs = 50'000'000;
}  // namespace

extern "C" void __wrap_svcSleepThread(s64 nano)
{
    if (EndHere())
        threadExit();
    while (nano > kSliceNs) {   // (a yield, 0 and under, and a short sleep go as they are)
        __real_svcSleepThread(kSliceNs);
        nano -= kSliceNs;
        if (EndHere())
            threadExit();
    }
    __real_svcSleepThread(nano);
    if (EndHere())
        threadExit();
}

extern "C" Result __wrap_svcWaitSynchronization(s32* index, const Handle* handles, s32 count, u64 timeout)
{
    if (EndHere())
        threadExit();
    Result rc;
    if (timeout <= (u64)kSliceNs) {
        rc = __real_svcWaitSynchronization(index, handles, count, timeout);
    } else {   // (in slices, so that the end is seen; the kernel's "timed out" until the whole time has passed)
        const u64 start = armGetSystemTick();
        for (;;) {
            const u64 passed = armTicksToNs(armGetSystemTick() - start);
            const u64 left = timeout == UINT64_MAX ? (u64)kSliceNs : passed >= timeout ? 0 : std::min<u64>(timeout - passed, (u64)kSliceNs);
            rc = __real_svcWaitSynchronization(index, handles, count, left);
            if (rc != 0xEA01 || (timeout != UINT64_MAX && passed + left >= timeout))
                break;
            if (EndHere())
                threadExit();
        }
    }
    if (EndHere())
        threadExit();
    return rc;
}

extern "C" Result __wrap_condvarWaitTimeout(CondVar* c, Mutex* m, u64 timeout)
{
    if (EndHere()) {
        mutexUnlock(m);
        threadExit();
    }
    const int slot = NoteWait(c);
    const Result rc = __real_condvarWaitTimeout(c, m, timeout);
    if (slot >= 0)
        g_waitingOn[slot].store(nullptr, std::memory_order_release);
    if (EndHere()) {
        mutexUnlock(m);
        threadExit();
    }
    return rc;
}

// The other threads told to end, and waited for up to `ms`: how many of those made have ended.
// The ones that have not are said, by their first function (addr2line on sly1.elf) and argument.
int EndOtherThreads(int ms, int* made)
{
    g_threadsEnd.store(true);
    const Handle self = threadGetCurHandle();
    int ended = 0, count = 0;
    for (int round = 0; round <= ms / 5; round++) {
        for (auto& waiting : g_waitingOn)
            if (CondVar* c = waiting.load())
                svcSignalProcessWideKey(c, -1);
        ended = count = 0;
        for (int k = 0; k < kThreadsMost; k++) {
            if (!g_threads[k].load() || g_threadHandle[k] == self)
                continue;
            count++;
            if (Ended(k))
                ended++;
        }
        if (ended == count)
            break;
        __real_svcSleepThread(5'000'000);
    }
    if (ended != count)
        for (int k = 0; k < kThreadsMost; k++) {
            if (g_threads[k].load() && g_threadHandle[k] != self && !Ended(k))
                std::fprintf(stderr, "sly1: a thread has not ended: its first function +0x%llX, argument 0x%llX\n",
                             (unsigned long long)(g_threadEntry[k] - (std::uintptr_t)ImageBase()), (unsigned long long)g_threadArg[k]);
        }
    if (made)
        *made = count;
    return ended;
}

// Every other thread of the program stopped where it is, for good, and its stack's mapping and
// handle given back: before the program ends with another to follow it (SLY1_NEXT). The next
// program is loaded into this process: a thread of this one that went on ran code no longer
// there, and a stack still mapped is memory of this program's image the loader cannot take back.
// The allocator's lock and the two output streams' are taken before the threads are stopped and
// kept, so that none is stopped holding one (the caller frees and closes after this). Called by the first
// thread (main). Returns how many were stopped.
extern "C" void __malloc_lock(struct _reent*);
extern "C" void __malloc_unlock(struct _reent*);
int StopOtherThreads()
{
    const Handle self = threadGetCurHandle();
    int stopped = 0;
    __malloc_lock(_REENT);
    flockfile(stdout);
    flockfile(stderr);
    for (int k = 0; k < kThreadsMost; k++)
        if (g_threads[k].load() && g_threadHandle[k] != self && !Ended(k) &&
            R_SUCCEEDED(svcSetThreadActivity(g_threadHandle[k], ThreadActivity_Paused)))
            stopped++;
    // (The three locks are kept: given up, each would pass to a thread that waited for it, which is
    // stopped now. They are this thread's as often as it asks, and the next program has its own.)
    for (int k = 0; k < kThreadsMost; k++) {
        if (!g_threads[k].exchange(nullptr) || g_threadHandle[k] == self)
            continue;
        // The stack's mapping taken away (its size asked of the kernel: libnx maps the stack with
        // the thread's own data after it, more than stack_sz) and the handle closed; each one
        // that does not go is said, with the kernel's answer. (One whose Thread was closed in
        // the meantime has no mapping left: the kernel then names a region that is not a stack.)
        MemoryInfo info{};
        u32 page = 0;
        Result rc = svcQueryMemory(&info, &page, (u64)g_threadMirror[k]);
        if (R_SUCCEEDED(rc) && info.type == MemType_MappedMemory && info.addr == (u64)g_threadMirror[k]) {
            rc = svcUnmapMemory(g_threadMirror[k], g_threadStack[k], info.size);
            if (R_FAILED(rc)) {
                char line[200];
                std::snprintf(line, sizeof line, "sly1: a stack's mapping stays: answer 0x%X; mirror %p (%llu KB, attribute 0x%X), stack %p\n", rc,
                              g_threadMirror[k], (unsigned long long)(info.size >> 10), info.attr, g_threadStack[k]);
                write(STDERR_FILENO, line, __builtin_strlen(line));
            }
        }
        svcCloseHandle(g_threadHandle[k]);
    }
    return stopped;
}

// The parts of the program's heap that are not plain memory of its own (not readable and
// writable, or with an attribute: lent to a service, mapped as a thread's stack, locked for a
// call), said in one line each. At the start of a run there should be none; one that a run before
// it in this process left is what the next run faults on (a transfer memory still with its
// service, 2026-11-25).
int HeapReport(const char* when, bool each)
{
    u64 base = 0, size = 0;
    if (envHasHeapOverride()) {
        base = (u64)envGetHeapOverrideAddr();
        size = envGetHeapOverrideSize();
    } else {
        svcGetInfo(&base, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&size, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);
    }
    int odd = 0;
    u64 at = base;
    char line[200];
    while (at < base + size) {
        MemoryInfo info;
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&info, &page, at)) || info.size == 0)
            break;
        const u64 end = std::min<u64>(info.addr + info.size, base + size);
        if ((info.perm != Perm_Rw || info.attr != 0) && odd++ < 24 && each) {
            std::snprintf(line, sizeof line, "heapmap: %s: +0x%llX, %llu KB: type 0x%X, attribute 0x%X, permission %u, lent to devices %u, to calls %u\n", when,
                          (unsigned long long)(at - base), (unsigned long long)((end - at) >> 10), info.type, info.attr, info.perm, info.device_refcount,
                          info.ipc_refcount);
            write(STDERR_FILENO, line, __builtin_strlen(line));
        }
        at = end;
    }
    std::snprintf(line, sizeof line, "heapmap: %s: %d parts of the heap (%llu MB at 0x%llX) not plain memory\n", when, odd, (unsigned long long)(size >> 20),
                  (unsigned long long)base);
    write(STDERR_FILENO, line, __builtin_strlen(line));
    return odd;
}

void PlaceThread(ThreadRole role, bool sampled)
{
    CheckDevTable("a thread placed");
    // SLY1_SAMPLE=4: the game's own threads in the render and GS threads' places (the first two
    // placed: the report's "render" and "gs" are then those)
    static const bool gameSampled = std::getenv("SLY1_SAMPLE") && std::getenv("SLY1_SAMPLE")[0] == '4';
    if (sampled && !gameSampled && (role == ThreadRole::Render || role == ThreadRole::Gs))
        g_sampled[role == ThreadRole::Gs ? 1 : 0] = threadGetCurHandle();
    if (gameSampled && role == ThreadRole::Game) {
        static std::atomic<int> next{0};
        const int slot = next.fetch_add(1);
        if (slot < 2)
            g_sampled[slot] = threadGetCurHandle();
        std::fprintf(stderr, "sly1: the sampler's place %d is a game thread\n", slot);
    }
    if (role == ThreadRole::Helper) {
        // The render thread's helper (the parts' hashes): the fourth core where the system lets
        // the program have it (it does not on the Switch as homebrew runs), else the game's core
        // beside the game's threads (they leave about half of it idle). SLY1_HELPER_CORE=N says
        // which.
        const char* asked = std::getenv("SLY1_HELPER_CORE");
        int core = asked ? std::atoi(asked) : 3;
        Result r = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
        if (R_FAILED(r)) {
            core = 0;
            r = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 1u);
        }
        // Below the game's threads on their core: it runs in what they leave idle and never holds
        // them up (at their own priority, frames that just made a vertical blank missed it: 825 of
        // the opening's frames in one blank where 1,013 had been). SLY1_HELPER_PRIORITY=N (hex).
        const char* prio = std::getenv("SLY1_HELPER_PRIORITY");
        const Result p = svcSetThreadPriority(CUR_THREAD_HANDLE, prio ? (s32)std::strtol(prio, nullptr, 16) : core == 3 ? 0x2C : 0x2D);
        std::fprintf(stderr, "sly1: the helper thread on core %d (0x%X, priority 0x%X)\n", core, (unsigned)r, (unsigned)p);
        return;
    }
    int core = role == ThreadRole::Game ? 0 : role == ThreadRole::Clock || role == ThreadRole::Gs ? 1 : 2;
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    s32 prio = 0x2C;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (role != ThreadRole::Game && role != ThreadRole::Render && role != ThreadRole::Gs)
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);   // above the game's (libnx gives 0x2C to main)
    else
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
}

// Where every other thread is, for a report of a hang or a fault (port/crashlog.h): each is paused
// for the moment its registers are read, and a frame record is followed only where the memory can
// be read. No heap is used.
namespace {
bool Readable(std::uintptr_t at)
{
    MemoryInfo info = {};
    u32 page = 0;
    return at % 8 == 0 && R_SUCCEEDED(svcQueryMemory(&info, &page, at)) && (info.perm & Perm_R) && at + 16 <= info.addr + info.size;
}
}
void DescribeThreads(void (*line)(const char*))
{
    const Handle self = threadGetCurHandle();
    const std::uintptr_t base = ImageBase(), size = ImageSize();
    for (int k = 0; k < kThreadsMost; k++) {
        if (!g_threads[k].load() || g_threadHandle[k] == self || Ended(k))
            continue;
        const Handle h = g_threadHandle[k];
        ThreadContext ctx = {};
        const bool paused = R_SUCCEEDED(svcSetThreadActivity(h, ThreadActivity_Paused));
        const Result got = svcGetThreadContext3(&ctx, h);
        char text[512];
        int n = std::snprintf(text, sizeof text, "thread %d (first function +0x%llX):", k, (unsigned long long)(g_threadEntry[k] - base));
        if (R_FAILED(got)) {
            std::snprintf(text + n, sizeof text - n, " its registers could not be read (0x%X)", got);
        } else {
            auto place = [&](const char* what, std::uintptr_t at) {
                if (at - base < size)
                    n += std::snprintf(text + n, sizeof text - n, " %s +0x%llX", what, (unsigned long long)(at - base));
                else
                    n += std::snprintf(text + n, sizeof text - n, " %s 0x%llX (not the program's)", what, (unsigned long long)at);
            };
            place("at", ctx.pc.x);
            place("lr", ctx.lr);
            std::uintptr_t fp = ctx.fp;
            for (int i = 0; i < 10 && fp && Readable(fp) && n < (int)sizeof text - 40; i++) {
                const std::uintptr_t ret = reinterpret_cast<std::uintptr_t*>(fp)[1];
                if (ret - base >= size)
                    break;
                n += std::snprintf(text + n, sizeof text - n, ", from +0x%llX", (unsigned long long)(ret - base));
                fp = reinterpret_cast<std::uintptr_t*>(fp)[0];
            }
        }
        if (paused)
            svcSetThreadActivity(h, ThreadActivity_Runnable);
        line(text);
    }
}
void PauseOtherThreads(bool pause)
{
    const Handle self = threadGetCurHandle();
    for (int k = 0; k < kThreadsMost; k++)
        if (g_threads[k].load() && g_threadHandle[k] != self && !Ended(k))
            svcSetThreadActivity(g_threadHandle[k], pause ? ThreadActivity_Paused : ThreadActivity_Runnable);
}

#else

void PlaceThread(ThreadRole, bool) {}
void StartSampler() {}
void SampleWindow(bool) {}
void SamplerReport() {}

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
