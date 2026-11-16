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
#include "vu1_handlers.h"

#include <string>
#include <vector>

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

namespace {

// The microprogram, interpreted (through the translated chunks where installed).
std::uint64_t Interpret(std::uint32_t start, const Context& ctx)
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
        } else {
            core::Step(r, pc);
        }
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

// The handler registered for the entry of the chunk installed there, or none.
const HandlerEntry* HandlerFor(std::uint32_t start)
{
    int o = g_owner[start & 0x7FF];
    if (o < 0)
        return nullptr;
    std::uint32_t hash = core::kTranslated[o].hash;
    for (std::size_t i = 0; i < kHandlerCount; i++)
        if (kHandlers[i].chunkHash == hash && kHandlers[i].entry == (start & 0x7FF))
            return &kHandlers[i];
    return nullptr;
}

// A packet as XGKICK sends it: its quadwords from qaddr through the tag with EOP.
std::vector<std::uint32_t> PacketAt(std::uint32_t qaddr)
{
    std::vector<std::uint32_t> out;
    const std::uint8_t* d = Data();
    for (int tags = 0; tags < 1024; tags++) {
        const std::uint32_t* tag = reinterpret_cast<const std::uint32_t*>(d + (qaddr & 0x3FF) * 16);
        std::uint32_t nloop = tag[0] & 0x7FFF, flg = (tag[1] >> 26) & 3, nreg = (tag[1] >> 28) & 0xF;
        if (nreg == 0)
            nreg = 16;
        std::uint32_t n = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        for (std::uint32_t k = 0; k <= n && k < 1024; k++) {
            const std::uint32_t* q = reinterpret_cast<const std::uint32_t*>(d + ((qaddr + k) & 0x3FF) * 16);
            out.insert(out.end(), q, q + 4);
        }
        qaddr += n + 1;
        if ((tag[0] >> 15) & 1)
            break;
    }
    return out;
}

struct Capture {
    std::vector<std::vector<std::uint32_t>> packets;
    const Context* real = nullptr;   // passed on to it too, when set
};

void CaptureKick(std::uint32_t qaddr, void* user)
{
    Capture* c = static_cast<Capture*>(user);
    c->packets.push_back(PacketAt(qaddr));
    if (c->real && c->real->kick)
        c->real->kick(qaddr, c->real->user);
}

struct Snapshot {
    State state;
    std::vector<std::uint8_t> data;
    void Take() { state = Vu1(); data.assign(Data(), Data() + 0x4000); }
    void Restore() const { Vu1() = state; std::memcpy(Data(), data.data(), 0x4000); }
};

std::uint64_t g_checked, g_checkFailures;

// The first difference between the handler's results and the interpreter's, or empty.
std::string Compare(const Snapshot& h, const Capture& hc, const Snapshot& i, const Capture& ic)
{
    char buf[200];
    for (int r = 0; r < 32; r++)
        for (int k = 0; k < 4; k++)
            if (std::memcmp(&h.state.vf[r][k], &i.state.vf[r][k], 4)) {
                std::snprintf(buf, sizeof buf, "vf%02d.%c: handler %08X, interpreter %08X", r, "xyzw"[k],
                              core::Bits(h.state.vf[r][k]), core::Bits(i.state.vf[r][k]));
                return buf;
            }
    for (int r = 1; r < 16; r++)
        if (h.state.vi[r] != i.state.vi[r]) {
            std::snprintf(buf, sizeof buf, "vi%02d: handler %04X, interpreter %04X", r, h.state.vi[r], i.state.vi[r]);
            return buf;
        }
    for (int k = 0; k < 4; k++)
        if (std::memcmp(&h.state.acc[k], &i.state.acc[k], 4))
            return std::string("ACC.") + "xyzw"[k];
    if (std::memcmp(&h.state.q, &i.state.q, 4)) return "Q";
    if (std::memcmp(&h.state.p, &i.state.p, 4)) return "P";
    if (std::memcmp(&h.state.i, &i.state.i, 4)) return "I";
    if (h.state.mac != i.state.mac) return "the MAC flags";
    if ((h.state.status & 0xFFF) != (i.state.status & 0xFFF)) return "the status flags";
    if (h.state.clip != i.state.clip) return "the clip flags";
    if (h.state.pc != i.state.pc) return "the end pc";
    for (std::size_t k = 0; k < h.data.size(); k += 4)
        if (std::memcmp(&h.data[k], &i.data[k], 4)) {
            std::uint32_t a, b;
            std::memcpy(&a, &h.data[k], 4);
            std::memcpy(&b, &i.data[k], 4);
            std::snprintf(buf, sizeof buf, "data memory at quadword 0x%03zX word %zu: handler %08X, interpreter %08X",
                          k / 16, (k / 4) % 4, a, b);
            return buf;
        }
    if (hc.packets.size() != ic.packets.size()) {
        std::snprintf(buf, sizeof buf, "packets kicked: handler %zu, interpreter %zu", hc.packets.size(), ic.packets.size());
        return buf;
    }
    for (std::size_t p = 0; p < hc.packets.size(); p++)
        if (hc.packets[p] != ic.packets[p]) {
            std::snprintf(buf, sizeof buf, "packet %zu differs", p);
            return buf;
        }
    return std::string();
}

}  // namespace

std::uint64_t Run(std::uint32_t start, const Context& ctx)
{
    const HandlerEntry* h = HandlerFor(start);
    static const bool check = std::getenv("SLY1_VU1_CHECK") != nullptr;
    if (!h)
        return Interpret(start, ctx);
    if (!check) {
        Runner r(ctx);
        h->fn(r);
        return 0;
    }
    // Both, from the same state: the handler's results kept aside, the interpreter's kept.
    Snapshot before;
    before.Take();
    Capture hc;
    Context hctx = ctx;
    hctx.kick = CaptureKick;
    hctx.user = &hc;
    {
        Runner r(hctx);
        h->fn(r);
    }
    Snapshot afterHandler;
    afterHandler.Take();
    before.Restore();
    Capture ic;
    ic.real = &ctx;
    Context ictx = ctx;
    ictx.kick = CaptureKick;
    ictx.user = &ic;
    std::uint64_t n = Interpret(start, ictx);
    Snapshot afterInterpreter;
    afterInterpreter.Take();
    g_checked++;
    std::string diff = Compare(afterHandler, hc, afterInterpreter, ic);
    if (!diff.empty() && g_checkFailures++ < 20)
        std::fprintf(stderr, "vu1 check: %s (entry 0x%03X): %s\n", h->name, start & 0x7FF, diff.c_str());
    return n;
}

std::uint64_t HandlerChecks(std::uint64_t* failures)
{
    if (failures)
        *failures = g_checkFailures;
    return g_checked;
}

std::uint64_t TranslatedInstructions() { return g_translatedRun; }

}  // namespace vu1
}  // namespace port
