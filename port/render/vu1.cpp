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
// old value (WAITQ and WAITP wait, and stall the upper operation beside them too). Register results otherwise follow in order, as the VU's
// interlocks make them.
#include "port/vu1.h"
#include "port/system.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vu1_runner.h"
#include "vu1_handlers.h"
#include "vu1_live.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace port {
namespace vu1 {

namespace {

using core::Runner;

}  // namespace

State& Vu1()
{
    if (State* lane = LaneState())
        return *lane;
    static State s;
    return s;
}
namespace {
// The micro memory and what runs it, as a lane's work found them when it was made (MicroNow):
// the render thread loads other microcode further down its list while the lane's part is run.
struct MicroImage {
    std::uint32_t micro[4096];
    core::ChunkFn table[2048];
    int owner[2048];
};
const MicroImage* Image() { return static_cast<const MicroImage*>(LaneMicro()); }
}  // namespace
std::uint32_t* Micro()
{
    if (const MicroImage* image = Image())
        return const_cast<std::uint32_t*>(image->micro);
    return reinterpret_cast<std::uint32_t*>(Vu1Memory());
}
std::uint8_t* Data()
{
    if (std::uint8_t* lane = LaneData())
        return lane;
    return Vu1Memory() + 0x4000;
}

namespace {

// What runs each instruction of micro memory: the translated chunk an upload installed, or none.
core::ChunkFn g_table[2048];
int g_owner[2048];   // the index of the chunk installed there, or -1
bool g_tableReady;
bool g_microChanged = true;   // since MicroNow last made or found its image
const core::ChunkFn* Table() { const MicroImage* image = Image(); return image ? image->table : g_table; }
const int* Owner() { const MicroImage* image = Image(); return image ? image->owner : g_owner; }
std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t> g_profile;   // SLY1_VU1_PROFILE
std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t> g_profileCalls;
std::uint64_t g_handlerNs[64], g_handlerCalls[64], g_fallbacks[64], g_handlerKickNs[64];
// (SLY1_VU1_PROFILE) the render thread's own: what its programs cost it, apart from the jobs' runs
std::atomic<std::uint64_t> g_renderHandlerNs[64], g_renderHandlerCalls[64];
thread_local bool t_renderThread;

// SLY1_VU1_PROFILE: a kick passed on and timed, to tell the GS's share from the handler's.
struct TimedKick {
    const Context* real;
    std::uint64_t* ns;
};
void KickTimed(std::uint32_t qaddr, void* user)
{
    TimedKick* t = static_cast<TimedKick*>(user);
    std::uint64_t t0 = NowNs();
    if (t->real->kick)
        t->real->kick(qaddr, t->real->user);
    *t->ns += NowNs() - t0;
}
std::map<std::uint32_t, std::uint64_t> g_stretch;
std::uint64_t g_translatedRun;

std::uint32_t HashWords(const std::uint32_t* w, std::size_t n)
{
    std::uint32_t h = 2166136261u;
    for (std::size_t k = 0; k < n; k++)
        h = (h ^ w[k]) * 16777619u;
    return h;
}

}  // namespace

const void* MicroNow()
{
    static std::map<std::uint64_t, MicroImage*> images;
    static const MicroImage* now = nullptr;
    if (!g_microChanged && now)
        return now;
    const std::uint32_t* micro = reinterpret_cast<const std::uint32_t*>(Vu1Memory());
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (int k = 0; k < 4096; k += 2) {
        std::uint64_t v;
        std::memcpy(&v, micro + k, 8);
        h = (h ^ v) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    MicroImage*& image = images[h];
    if (!image) {
        image = new MicroImage;
        std::memcpy(image->micro, micro, sizeof image->micro);
        std::memcpy(image->table, g_table, sizeof image->table);
        if (g_tableReady)
            std::memcpy(image->owner, g_owner, sizeof image->owner);
        else
            for (int& o : image->owner)
                o = -1;
    }
    g_microChanged = false;
    now = image;
    return now;
}

void MicrocodeLoaded(std::uint32_t addr, std::uint32_t count)
{
    g_microChanged = true;
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
    static const bool tracking = live::Enabled();
    static const char* profileEnv = std::getenv("SLY1_VU1_PROFILE");
    static const bool stretches = profileEnv && profileEnv[0] == '2';   // per 16 instructions
    static const bool interpretOnly = tracking || stretches || std::getenv("SLY1_VU1_INTERPRET") != nullptr;
    if (tracking) {
        int o = Owner()[pc];
        live::Begin(pc, o < 0 ? 0 : core::kTranslated[o].hash);
    }
    std::uint64_t translated = 0;
    const core::ChunkFn* table = Table();
    while (!r.done) {
        if (stretches)
            g_stretch[pc & ~15u]++;
        if (tracking)
            live::Instruction(pc, Micro()[pc * 2 + 1], Micro()[pc * 2], r.cycle);
        if (!interpretOnly && r.branch < 0 && !r.ending && table[pc]) {
            std::uint64_t before = r.executed;
            pc = table[pc](r, pc);
            translated += r.executed - before;
        } else {
            core::Step(r, pc);
        }
        if (r.executed > (LaneState() ? 1u << 20 : 1u << 24)) {
            // (in a lane: a job run again elsewhere may be left reading a packet the game has
            // since written over; what it makes is dropped, and it need only end)
            if (LaneState())
                break;
            std::fprintf(stderr, "port: a VU1 microprogram from 0x%03X ran without ending\n", start);
            std::abort();
        }
    }
    if (!LaneState())
        g_translatedRun += translated;
    static const bool profile = std::getenv("SLY1_VU1_PROFILE") != nullptr;
    if (profile) {
        int o = Owner()[start & 0x7FF];
        g_profile[{o < 0 ? 0u : core::kTranslated[o].hash, start & 0x7FF}] += r.executed;
        g_profileCalls[{o < 0 ? 0u : core::kTranslated[o].hash, start & 0x7FF}]++;
        // the instructions run in each 16-instruction stretch of micro memory

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
    int o = Owner()[start & 0x7FF];
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

// The first difference between the handler's results and the interpreter's, or empty: in what
// the entry leaves for later programs (its registers named in leaves), the end pc, data memory
// and the packets kicked.
std::string Compare(const Snapshot& h, const Capture& hc, const Snapshot& i, const Capture& ic, const live::Set& leaves)
{
    char buf[200];
    auto word = [](const State& st, int bit) -> std::uint32_t {
        if (bit < live::kVi)
            return core::Bits(st.vf[bit / 4][bit % 4]);
        if (bit < live::kAcc)
            return st.vi[bit - live::kVi];
        if (bit < live::kQ)
            return core::Bits(st.acc[bit - live::kAcc]);
        switch (bit) {
        case live::kQ: return core::Bits(st.q);
        case live::kP: return core::Bits(st.p);
        case live::kI: return core::Bits(st.i);
        case live::kMac: return st.mac & 0xFFFF;
        case live::kStatus: return st.status & 0x3F;
        case live::kSticky: return st.status & 0xFC0;
        default: return st.clip & 0xFFFFFF;
        }
    };
    for (int b = 0; b < live::kCount; b++) {
        if (!leaves[b])
            continue;
        std::uint32_t a = word(h.state, b), c = word(i.state, b);
        if (a != c) {
            char name[16];
            std::snprintf(buf, sizeof buf, "%s: handler %08X, interpreter %08X", live::Name(b, name, sizeof name), a, c);
            return buf;
        }
    }
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

namespace {
thread_local std::uint32_t g_currentHash, g_currentEntry;
}

void CurrentProgram(std::uint32_t* hash, std::uint32_t* entry)
{
    *hash = g_currentHash;
    *entry = g_currentEntry;
}

std::uint64_t Run(std::uint32_t start, const Context& ctx)
{
    {
        int o = Owner()[start & 0x7FF];
        g_currentHash = o < 0 ? 0 : core::kTranslated[o].hash;
        g_currentEntry = start & 0x7FF;
        static const bool seq = std::getenv("SLY1_VU1_SEQ") != nullptr;   // the programs in order, for diagnosis
        static long printed;
        if (seq && printed++ < 400000) {
            const HandlerEntry* hh = HandlerFor(start);
            std::fprintf(stderr, "seq %08X 0x%03X top %u %s\n", g_currentHash, g_currentEntry, ctx.top, hh ? hh->name : "-");
        }
    }
    static const bool off = std::getenv("SLY1_VU1_NO_HANDLERS") != nullptr;
    const HandlerEntry* h = live::Enabled() || off ? nullptr : HandlerFor(start);
    static const bool check = std::getenv("SLY1_VU1_CHECK") != nullptr;
    if (!check) {   // a transform noted for the GPU renderer, done now unless its kicks follow
        Runner r(ctx);
        BeforeProgram(h ? h->fn : nullptr, r);
    }
    if (!h) {
        // SLY1_PROJ_DUMP=N: the first N runs of 0x750, 0x752 and 0x754 (chunk 51A89CD8, the
        // projected texture), with what they read and what they change of VU1's memory
        static const int projDump = std::getenv("SLY1_PROJ_DUMP") ? std::atoi(std::getenv("SLY1_PROJ_DUMP")) : 0;
        static int projSaid;
        const std::uint32_t e = start & 0x7FF;
        if ((e == 0x750 || e == 0x752 || e == 0x754) && g_currentHash == 0x51A89CD8u && projSaid >= projDump) {
            const std::uint64_t res = Interpret(start, ctx);
            SwayNoted(ctx.top, e);
            return res;
        }
        if (projSaid < projDump && (e == 0x750 || e == 0x752 || e == 0x754)) {
            projSaid++;
            static std::uint8_t was[16384];
            std::uint8_t* d = Data();
            std::memcpy(was, d, sizeof was);
            const std::uint64_t res = Interpret(start, ctx);
            const std::uint32_t top = ctx.top & 0x3FF;
            auto q = [&](const std::uint8_t* m, std::uint32_t a) { return reinterpret_cast<const float*>(m + ((a & 0x3FF) * 16)); };
            std::fprintf(stderr, "proj: entry 0x%03X top %u; header 4-7, 11, 13, 22-27 before:\n", e, top);
            for (std::uint32_t k : {4u, 5u, 6u, 7u, 11u, 13u, 22u, 23u, 24u, 25u, 26u, 27u}) {
                const float* f = q(was, top + k);
                const std::uint32_t* w = reinterpret_cast<const std::uint32_t*>(f);
                std::fprintf(stderr, "proj:   %2u: %g %g %g %g (%08X %08X %08X %08X)\n", k, f[0], f[1], f[2], f[3], w[0], w[1], w[2], w[3]);
            }
            int shown = 0;
            for (std::uint32_t a = 0; a < 1024 && shown < 24; a++)
                if (std::memcmp(was + a * 16, d + a * 16, 16) != 0) {
                    shown++;
                    const float* b = q(was, a), * n = q(d, a);
                    std::fprintf(stderr, "proj:   changed %4u (top+%d): %g %g %g %g -> %g %g %g %g\n", a, (int)a - (int)top, b[0], b[1], b[2], b[3], n[0], n[1], n[2], n[3]);
                }
            if (g_currentHash == 0x51A89CD8u)
                SwayNoted(ctx.top, e);
            return res;
        }
        return Interpret(start, ctx);
    }
    if (!check) {
        Runner r(ctx);
        static const bool profile = std::getenv("SLY1_VU1_PROFILE") != nullptr;
        if (!profile) {
            if (h->fn(r))
                return 0;
            LeftToMicrocode(r);
            return Interpret(start, ctx);
        }
        TimedKick tk{&ctx, &g_handlerKickNs[h - kHandlers]};
        Context timed = ctx;
        timed.kick = KickTimed;
        timed.user = &tk;
        Runner rt(timed);
        std::uint64_t t0 = NowNs();
        bool done = h->fn(rt);
        if (!done) {
            g_fallbacks[h - kHandlers]++;
            LeftToMicrocode(rt);
        }
        const std::uint64_t took = NowNs() - t0;
        g_handlerNs[h - kHandlers] += took;
        g_handlerCalls[h - kHandlers]++;
        if (t_renderThread) {
            g_renderHandlerNs[h - kHandlers].fetch_add(took, std::memory_order_relaxed);
            g_renderHandlerCalls[h - kHandlers].fetch_add(1, std::memory_order_relaxed);
        }
        return done ? 0 : Interpret(start, ctx);
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
        if (!h->fn(r)) {   // a case the handler leaves to the microcode
            g_fallbacks[h - kHandlers]++;
            before.Restore();
            return Interpret(start, ctx);
        }
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
    static std::vector<live::Set> leaves;
    if (leaves.empty())
        for (std::size_t k = 0; k < kHandlerCount; k++)
            leaves.push_back(live::Parse(kHandlers[k].leaves));
    std::string diff = Compare(afterHandler, hc, afterInterpreter, ic, leaves[h - kHandlers]);
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

void ReportLive()
{
    {
        std::uint64_t taken, notTaken;
        BatchCounts(&taken, &notTaken);
        if (taken || notTaken)
            std::fprintf(stderr, "vu1: %llu kicks of batches taken by the GPU renderer, %llu not taken (transformed by the program)\n",
                         (unsigned long long)taken, (unsigned long long)notTaken);
    }
    if (live::Enabled())
        live::Report();
    std::vector<std::pair<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>>> rows;
    std::uint64_t total = 0;
    for (const auto& e : g_profile) {
        rows.push_back({e.second, e.first});
        total += e.second;
    }
    std::sort(rows.rbegin(), rows.rend());
    for (std::size_t k = 0; k < kHandlerCount && k < 64; k++)
        if (g_handlerCalls[k])
            std::fprintf(stderr, "vu1 profile: handler %s: %llu calls (%llu left to the microcode), %.3f s, %.3f s of it in kicks\n",
                         kHandlers[k].name, (unsigned long long)g_handlerCalls[k], (unsigned long long)g_fallbacks[k],
                         g_handlerNs[k] / 1e9, g_handlerKickNs[k] / 1e9);
    for (const auto& row : rows)
        std::fprintf(stderr, "vu1 profile: chunk %08X entry 0x%03X: %llu instructions (%.1f%%) in %llu calls\n",
                     row.second.first, row.second.second, (unsigned long long)row.first,
                     total ? row.first * 100.0 / total : 0.0, (unsigned long long)g_profileCalls[row.second]);
    for (const auto& e : g_stretch)
        if (e.second * 200 > total)
            std::fprintf(stderr, "vu1 profile: 0x%03X-0x%03X: %.1f%%\n", e.first, e.first + 15, e.second * 100.0 / total);
}

std::uint64_t TranslatedInstructions() { return g_translatedRun; }

void MarkRenderThread() { t_renderThread = true; }

std::uint64_t RenderHandlerNs()
{
    std::uint64_t sum = 0;
    for (std::size_t k = 0; k < kHandlerCount && k < 64; k++)
        sum += g_renderHandlerNs[k].load(std::memory_order_relaxed);
    return sum;
}

void RenderProfileLine(std::uint64_t shown, double frames)
{
    static std::uint64_t was[64], callsWas[64];
    std::vector<std::pair<std::uint64_t, std::size_t>> rows;
    std::uint64_t all = 0;
    for (std::size_t k = 0; k < kHandlerCount && k < 64; k++) {
        const std::uint64_t now = g_renderHandlerNs[k].load(std::memory_order_relaxed);
        rows.push_back({now - was[k], k});
        all += now - was[k];
    }
    std::sort(rows.rbegin(), rows.rend());
    std::fprintf(stderr, "perfvu1: to %llu, a frame: the render thread's programs %.2f ms;", (unsigned long long)shown, frames > 0 ? all / frames / 1e6 : 0.0);
    for (std::size_t r = 0; r < rows.size() && r < 8 && rows[r].first > 0; r++) {
        const std::size_t k = rows[r].second;
        std::fprintf(stderr, " %s %.2f (%.0f)", kHandlers[k].name, rows[r].first / frames / 1e6,
                     (g_renderHandlerCalls[k].load(std::memory_order_relaxed) - callsWas[k]) / frames);
    }
    std::fprintf(stderr, "\n");
    for (std::size_t k = 0; k < kHandlerCount && k < 64; k++) {
        was[k] = g_renderHandlerNs[k].load(std::memory_order_relaxed);
        callsWas[k] = g_renderHandlerCalls[k].load(std::memory_order_relaxed);
    }
}

}  // namespace vu1
}  // namespace port
