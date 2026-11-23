// SPDX-License-Identifier: AGPL-3.0-or-later
// SLY1_PROFILE=1 (the Switch): a sampling profiler of one thread. A thread of its own, on the render
// thread's core at a high priority, pauses the profiled thread about a thousand times a second,
// reads its program counter and link register, and lets it go on. Every 20 seconds it prints what
// it gathered as offsets from the program's base (prof: lines; tools/prof_resolve.py names them
// from the ELF). Nothing is allocated while the thread is paused (it may hold the allocator's lock).

#include "port/system.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace port {

#if defined(__SWITCH__)
namespace {

Handle g_profiled = INVALID_HANDLE;
Thread g_sampler;
constexpr std::uint32_t kSamples = 1u << 16;
std::uint64_t g_pc[kSamples], g_lr[kSamples];

void Sampler(void*)
{
    const std::uintptr_t base = (std::uintptr_t)&ProfileThisThread;
    std::fprintf(stderr, "prof: base 0x%llX (ProfileThisThread)\n", (unsigned long long)base);
    std::uint32_t n = 0;
    std::uint64_t at = armGetSystemTick(), freq = armGetSystemTickFreq();
    for (;;) {
        svcSleepThread(1000000);
        if (R_SUCCEEDED(svcSetThreadActivity(g_profiled, ThreadActivity_Paused))) {
            ThreadContext ctx;
            const Result r = svcGetThreadContext3(&ctx, g_profiled);
            svcSetThreadActivity(g_profiled, ThreadActivity_Runnable);
            if (R_SUCCEEDED(r) && n < kSamples) {
                g_pc[n] = ctx.pc.x;
                g_lr[n] = ctx.lr;
                n++;
            }
        }
        const std::uint64_t now = armGetSystemTick();
        if (now - at >= freq * 20 || n == kSamples) {
            at = now;
            // pc:lr pairs, in lines of 24
            for (std::uint32_t k = 0; k < n; k += 24) {
                std::fprintf(stderr, "prof:");
                for (std::uint32_t j = k; j < n && j < k + 24; j++)
                    std::fprintf(stderr, " %llX:%llX", (unsigned long long)(g_pc[j] - base), (unsigned long long)(g_lr[j] - base));
                std::fprintf(stderr, "\n");
            }
            std::fprintf(stderr, "prof: %u samples\n", n);
            n = 0;
        }
    }
}

}  // namespace

void ProfileThisThread()
{
    static const bool on = std::getenv("SLY1_PROFILE") != nullptr;
    if (!on || g_profiled != INVALID_HANDLE)
        return;
    g_profiled = threadGetCurHandle();
    if (R_FAILED(threadCreate(&g_sampler, Sampler, nullptr, nullptr, 0x8000, 0x20, 2)) || R_FAILED(threadStart(&g_sampler)))
        std::fprintf(stderr, "prof: the sampler did not start\n");
}
#else
void ProfileThisThread() {}
#endif

}  // namespace port
