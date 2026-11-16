// SPDX-License-Identifier: AGPL-3.0-or-later
// VU1 in micro mode (port/vu1.h): an interpreter for the instructions the game's VU1 microcode
// uses, written from the public descriptions of the VU's instruction formats and operations, as
// the VU0 interpreter (port/platform/vu0_micro.cpp) is.
//
// Each 64-bit instruction is an upper (FMAC) and a lower operation, both reading the registers as
// they were before it; the lower's register write lands first and the upper's after it. VU
// arithmetic has no infinity, NaN or denormals (port/vu0_impl.h): an operand or a result with the
// largest exponent is the largest finite float with its sign, a denormal is 0; it rounds toward
// zero (the game thread's rounding mode). Branches have a delay slot; an instruction with the end
// bit is followed by one more.
//
// Timing, where the game's code is scheduled around it: the MAC, status and clip flags an
// operation sets are seen four instructions later; DIV, SQRT and RSQRT write Q after 7, 7 and 13,
// the EFU's operations write P after their latencies, and a read of Q or P before then sees the
// old value (WAITQ and WAITP wait). Register results otherwise follow in order, as the VU's
// interlocks make them.
#include "port/vu1.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vu1_runner.h"

namespace port {
namespace vu1 {

namespace {

using core::Runner;

}  // namespace

State& Vu1()
{
    static State s;
    return s;
}
std::uint32_t* Micro() { return reinterpret_cast<std::uint32_t*>(Vu1Memory()); }
std::uint8_t* Data() { return Vu1Memory() + 0x4000; }

namespace {

// What runs each instruction of micro memory: the translated chunk an upload installed, or none.
core::ChunkFn g_table[2048];
int g_owner[2048];   // the index of the chunk installed there, or -1
bool g_tableReady;
std::uint64_t g_translatedRun;

std::uint32_t HashWords(const std::uint32_t* w, std::size_t n)
{
    std::uint32_t h = 2166136261u;
    for (std::size_t k = 0; k < n; k++)
        h = (h ^ w[k]) * 16777619u;
    return h;
}

}  // namespace

void MicrocodeLoaded(std::uint32_t addr, std::uint32_t count)
{
    if (!g_tableReady) {
        for (int& o : g_owner)
            o = -1;
        g_tableReady = true;
    }
    // what the upload overwrites is no longer any chunk's
    for (std::uint32_t k = 0; k < count; k++) {
        int o = g_owner[(addr + k) & 0x7FF];
        if (o < 0)
            continue;
        const core::Chunk& c = core::kTranslated[o];
        for (std::uint32_t j = 0; j < c.count; j++) {
            std::uint32_t at = (c.addr + j) & 0x7FF;
            if (g_owner[at] == o) {
                g_owner[at] = -1;
                g_table[at] = nullptr;
            }
        }
    }
    std::uint32_t h = HashWords(Micro() + (addr & 0x7FF) * 2, (std::size_t)count * 2);
    for (std::size_t i = 0; i < core::kTranslatedCount; i++) {
        const core::Chunk& c = core::kTranslated[i];
        if (c.addr == (addr & 0x7FF) && c.count == count && c.hash == h) {
            for (std::uint32_t j = 0; j < count; j++) {
                g_table[(addr + j) & 0x7FF] = c.fn;
                g_owner[(addr + j) & 0x7FF] = (int)i;
            }
            break;
        }
    }
}

std::uint64_t Run(std::uint32_t start, const Context& ctx)
{
    State& s = Vu1();
    s.vf[0][0] = s.vf[0][1] = s.vf[0][2] = 0.0f;
    s.vf[0][3] = 1.0f;
    s.vi[0] = 0;
    Runner r(ctx);
    std::uint32_t pc = start & 0x7FF;
    static const bool interpretOnly = std::getenv("SLY1_VU1_INTERPRET") != nullptr;
    while (!r.done) {
        if (!interpretOnly && r.branch < 0 && !r.ending && g_table[pc]) {
            std::uint64_t before = r.executed;
            pc = g_table[pc](r, pc);
            g_translatedRun += r.executed - before;
        } else
            core::Step(r, pc);
        if (r.executed > (1u << 24)) {
            std::fprintf(stderr, "port: a VU1 microprogram from 0x%03X ran without ending\n", start);
            std::abort();
        }
    }
    s.pc = pc;
    r.cycle += 64;   // the program's last results settle
    r.Settle();
    r.Q();
    r.P();
    return r.executed;
}

std::uint64_t TranslatedInstructions() { return g_translatedRun; }

}  // namespace vu1
}  // namespace port
