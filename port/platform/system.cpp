// SPDX-License-Identifier: AGPL-3.0-or-later
// The platform layer's system state (port/system.h) and the VU0 microprogram stand-in.
#include "port/system.h"

#include <atomic>
#include "port/retail_data.h"

#include <chrono>
#if defined(__SWITCH__)
#include <switch.h>
#endif
#include <cstdio>
#include <cstdlib>

#include "port/vu0_impl.h"

namespace port {

std::uint32_t Cop0Status() { return 0x10001; }

std::uint64_t NowNs()
{
#if defined(__SWITCH__)
    return armTicksToNs(armGetSystemTick());
#else
    using namespace std::chrono;
    return (std::uint64_t)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

namespace {
std::atomic<bool> g_lockstep{false};
std::atomic<std::uint64_t> g_lockstepTicks{0}, g_lockstepCalls{0};
}

void SetLockstep(bool on) { g_lockstep = on; }
void LockstepTick() { g_lockstepTicks++; }

std::uint64_t CycleCount()
{
    if (g_lockstep)   // a vblank's cycles each (294,912,000 / 60), and 1,000 a call
        return g_lockstepTicks.load() * 4915200 + (g_lockstepCalls++ % 4000) * 1000;
    static const std::uint64_t start = NowNs();
    std::uint64_t ns = NowNs() - start;
    return ns * 294912 / 1000000;   // 294.912 MHz
}

}  // namespace port

namespace port_vu0 {

// The VU0 microprograms are not decompiled: their two callers (CollectLightsForSphere,
// FilterParticleVerticesVu0) get the port's own code. Until then, reaching one stops the game
// with its name.
void Microprogram(const char* what)
{
    std::fprintf(stderr, "port: VU0 microprogram interface %s reached; its caller is not ported yet\n", what);
    std::abort();
}

}  // namespace port_vu0

namespace port {
LevelStreamEnd g_levelStreamEnds[16];
std::atomic<int> g_levelStreamCloses{0};

void TraceBytes(const char* what, const void* p, int n)
{
    static const bool trace = std::getenv("SLY1_STREAM_TRACE") != nullptr;
    if (!trace)
        return;
    std::fprintf(stderr, "trace: %s at 0x%08X:", what, GameAddr(p));
    for (int i = 0; i + 4 <= n; i += 4)
        std::fprintf(stderr, " %08X", *reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(p) + i));
    std::fprintf(stderr, "\n");
}

void NoteLevelStreamEnd(int remaining, int bytesToRead, int nextCount, int carry)
{
    int n = g_levelStreamCloses.load() + 1;
    // ReadBufferedBlockHeader's g_bufferedBlockSize and g_bufferedBlockLoad
    g_levelStreamEnds[n % 16] = {remaining, bytesToRead, nextCount, carry, n,
                                 *(std::int32_t*)Native(0x275720), *(std::int32_t*)Native(0x27571C)};
    g_levelStreamCloses.store(n);
}
}  // namespace port
