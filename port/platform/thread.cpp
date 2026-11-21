// SPDX-License-Identifier: AGPL-3.0-or-later
// Threads on stacks in game memory (port/thread.h).
#include "port/thread.h"

#include <cstdint>
#include <algorithm>
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

void PlaceThread(ThreadRole role, bool sampled)
{
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
