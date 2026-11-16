// SPDX-License-Identifier: AGPL-3.0-or-later
// SLY1_VU1_LIVE (port/render/vu1_live.h): the instruction formats decoded for their operands only,
// as port/render/vu1_runner.h executes them. A register is written when its instruction runs (the
// VU's interlocks keep register results in order); the flags four instructions later; Q and P
// after their unit's latency, so a read before then still sees the program's input.
#include "vu1_live.h"

#include "vu1_runner.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>
#include <map>
#include <utility>

namespace port {
namespace vu1 {
namespace live {

namespace {

struct Program {
    Set written;
    Set inputs;
    // When the timed pieces' first write lands (a cycle), or never.
    std::uint64_t qAt, pAt, macAt, statusAt, clipAt;
    bool clipIssued;
};

constexpr std::uint64_t kNever = ~0ull;

Program g_now;
std::uint32_t g_entry, g_chunk, g_pc;
std::map<std::pair<std::uint32_t, std::uint32_t>, std::map<int, std::uint32_t>> g_where;   // first pc reading each
bool g_running;
std::map<std::pair<std::uint32_t, std::uint32_t>, Set> g_byEntry;
Set g_union;
// Which entry last wrote each piece, and for each writer the pieces a later entry read from it
// (with one reader each, for the report).
std::pair<std::uint32_t, std::uint32_t> g_lastWriter[kCount];
std::map<std::pair<std::uint32_t, std::uint32_t>, std::map<int, std::pair<std::uint32_t, std::uint32_t>>> g_leaves;

std::uint32_t g_history[64];
unsigned g_historyAt;
int g_traces;

void Read(int bit)
{
    if (!g_now.written[bit] && !g_now.inputs[bit]) {
        static const char* trace = std::getenv("SLY1_VU1_LIVE_TRACE");
        if (trace && (std::uint32_t)std::strtoul(trace, nullptr, 16) == g_entry && g_traces++ < 3) {
            std::fprintf(stderr, "vu1 live trace: entry 0x%03X piece %d at 0x%03X after:", g_entry, bit, g_pc);
            for (unsigned k = 0; k < 64 && k < g_historyAt; k++)
                std::fprintf(stderr, " %03X", g_history[(g_historyAt - std::min(g_historyAt, 64u) + k) & 63]);
            std::fputc('\n', stderr);
        }
        g_now.inputs[bit] = true;
        g_union[bit] = true;
        g_byEntry[{g_chunk, g_entry}][bit] = true;
        g_where[{g_chunk, g_entry}].emplace(bit, g_pc);
        g_leaves[g_lastWriter[bit]].emplace(bit, std::make_pair(g_chunk, g_entry));
    }
}

// A timed piece: its value is the program's own once its first write has landed.
void ReadTimed(int bit, std::uint64_t at, std::uint64_t cycle)
{
    if (at == kNever || at > cycle)
        Read(bit);
}

void Write(int bit) { g_now.written[bit] = true; }

void ReadVf(int r, int fields)   // fields: x 8, y 4, z 2, w 1
{
    if (r == 0)
        return;
    for (int k = 0; k < 4; k++)
        if (fields & (8 >> k))
            Read(kVf + r * 4 + k);
}
void WriteVf(int r, int fields)
{
    if (r == 0)
        return;
    for (int k = 0; k < 4; k++)
        if (fields & (8 >> k))
            Write(kVf + r * 4 + k);
}
void ReadVi(int r) { if (r & 15) Read(kVi + (r & 15)); }
void WriteVi(int r) { if (r & 15) Write(kVi + (r & 15)); }
void ReadAcc(int fields)
{
    for (int k = 0; k < 4; k++)
        if (fields & (8 >> k))
            Read(kAcc + k);
}
void WriteAcc(int fields)
{
    for (int k = 0; k < 4; k++)
        if (fields & (8 >> k))
            Write(kAcc + k);
}
int Field(int k) { return 8 >> k; }

void Upper(std::uint32_t w, std::uint64_t cycle)
{
    int op = w & 0x3F, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31;
    int dest = (w >> 21) & 0xF, bc = w & 3;
    bool flags = true, acc = false, macc = false;
    enum { Vec, Bc, Q, I } src = Vec;
    int to = fd;   // a VF register, or -1 for ACC
    if (op < 0x1C) {
        int kind = op >> 2;   // add sub madd msub max mini mul
        src = Bc;
        macc = kind == 2 || kind == 3;
        flags = kind != 4 && kind != 5;
    } else if (op < 0x3C) {
        switch (op) {
        case 0x1C: case 0x20: case 0x24: src = Q; break;
        case 0x21: case 0x25: src = Q; macc = true; break;
        case 0x1E: case 0x22: case 0x26: src = I; break;
        case 0x1D: case 0x1F: src = I; flags = false; break;
        case 0x23: case 0x27: src = I; macc = true; break;
        case 0x29: case 0x2D: macc = true; break;
        case 0x2B: case 0x2F: flags = false; break;
        case 0x2E:   // OPMSUB
            ReadVf(fs, 0xE);
            ReadVf(ft, 0xE);
            ReadAcc(dest & 0xE);
            WriteVf(fd, dest);
            g_now.macAt = std::min(g_now.macAt, cycle + 4);
            g_now.statusAt = std::min(g_now.statusAt, cycle + 4);
            return;
        }
    } else {
        int o2 = (((w >> 6) & 31) << 2) | (w & 3);
        acc = true;
        to = -1;
        if (o2 < 0x10) {
            src = Bc;
            macc = (o2 >> 2) >= 2;
        } else if (o2 < 0x18) {   // ITOF, FTOI
            ReadVf(fs, dest);
            WriteVf(ft, dest);
            return;
        } else if (o2 < 0x1C) {
            src = Bc;
        } else {
            switch (o2) {
            case 0x1C: case 0x20: case 0x24: src = Q; break;
            case 0x21: case 0x25: src = Q; macc = true; break;
            case 0x1E: case 0x22: case 0x26: src = I; break;
            case 0x23: case 0x27: src = I; macc = true; break;
            case 0x29: case 0x2D: macc = true; break;
            case 0x1D:   // ABS
                ReadVf(fs, dest);
                WriteVf(ft, dest);
                return;
            case 0x1F:   // CLIP: shifts the newest clip flags, whether seen yet or not
                ReadVf(fs, 0xE);
                ReadVf(ft, 0x1);
                if (!g_now.clipIssued)
                    Read(kClip);
                g_now.clipIssued = true;
                g_now.clipAt = std::min(g_now.clipAt, cycle + 4);
                return;
            case 0x2E:   // OPMULA
                ReadVf(fs, 0xE);
                ReadVf(ft, 0xE);
                WriteAcc(dest & 0xE);
                g_now.macAt = std::min(g_now.macAt, cycle + 4);
                g_now.statusAt = std::min(g_now.statusAt, cycle + 4);
                return;
            case 0x2F:   // NOP
                return;
            }
        }
    }
    ReadVf(fs, dest);
    if (src == Vec)
        ReadVf(ft, dest);
    else if (src == Bc)
        ReadVf(ft, Field(bc));
    else if (src == Q)
        ReadTimed(kQ, g_now.qAt, cycle);
    else
        Read(kI);
    if (macc)
        ReadAcc(dest);
    if (acc)
        WriteAcc(dest);
    else
        WriteVf(to, dest);
    if (flags) {
        g_now.macAt = std::min(g_now.macAt, cycle + 4);
        g_now.statusAt = std::min(g_now.statusAt, cycle + 4);
    }
}


void Lower(std::uint32_t w, std::uint64_t cycle)
{
    int op = (w >> 25) & 0x7F, it = (w >> 16) & 31, is = (w >> 11) & 31, id = (w >> 6) & 31;
    int dest = (w >> 21) & 0xF;
    std::uint32_t imm12 = ((w >> 10) & 0x800) | (w & 0x7FF);
    auto readStatus = [&](std::uint32_t mask) {
        if (mask & 0x00F)
            ReadTimed(kStatus, g_now.statusAt, cycle);
        if (mask & 0xFF0)
            Read(kSticky);
    };
    switch (op) {
    case 0x00: ReadVi(is); WriteVf(it, dest); return;                  // LQ
    case 0x01: ReadVf(is, dest); ReadVi(it); return;                   // SQ
    case 0x04: ReadVi(is); WriteVi(it); return;                        // ILW
    case 0x05: ReadVi(is); ReadVi(it); return;                         // ISW
    case 0x08: case 0x09: ReadVi(is); WriteVi(it); return;             // IADDIU, ISUBIU
    case 0x10: case 0x12: case 0x13: ReadTimed(kClip, g_now.clipAt, cycle); WriteVi(1); return;
    case 0x11: Write(kClip); g_now.clipAt = std::min(g_now.clipAt, cycle); g_now.clipIssued = true; return;
    case 0x14: case 0x16: case 0x17: readStatus(op == 0x14 ? 0xFFF : imm12); WriteVi(it); return;
    case 0x15: Write(kSticky); return;                                 // FSSET
    case 0x18: case 0x1A: case 0x1B: ReadTimed(kMac, g_now.macAt, cycle); ReadVi(is); WriteVi(it); return;
    case 0x1C: ReadTimed(kClip, g_now.clipAt, cycle); WriteVi(it); return;   // FCGET
    case 0x20: return;                                                 // B
    case 0x21: WriteVi(it); return;                                    // BAL
    case 0x24: ReadVi(is); return;                                     // JR
    case 0x25: ReadVi(is); WriteVi(it); return;                        // JALR
    case 0x28: case 0x29: ReadVi(it); ReadVi(is); return;              // IBEQ, IBNE
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: ReadVi(is); return;
    case 0x40: break;
    default: return;
    }
    int f = w & 0x3F;
    if (f >= 0x30 && f <= 0x35) {
        ReadVi(is);
        if (f == 0x32) {
            WriteVi(it);
        } else {
            ReadVi(it);
            WriteVi(id);
        }
        return;
    }
    if (f < 0x3C)
        return;
    int o2 = (((w >> 6) & 31) << 2) | (w & 3);
    int fsf = Field((w >> 21) & 3), ftf = Field((w >> 23) & 3);
    auto setQ = [&](int latency) { g_now.qAt = std::min(g_now.qAt, cycle + latency); };
    auto setP = [&](int latency) { g_now.pAt = std::min(g_now.pAt, cycle + latency); };
    switch (o2) {
    case 0x30: ReadVf(is, dest); WriteVf(it, dest); return;            // MOVE
    case 0x31: {                                                       // MR32
        int fields = 0;
        for (int k = 0; k < 4; k++)
            if (dest & (8 >> k))
                fields |= 8 >> ((k + 1) & 3);
        ReadVf(is, fields);
        WriteVf(it, dest);
        return;
    }
    case 0x34: ReadVi(is); WriteVf(it, dest); WriteVi(is); return;      // LQI
    case 0x35: ReadVf(is, dest); ReadVi(it); WriteVi(it); return;      // SQI
    case 0x36: ReadVi(is); WriteVi(is); WriteVf(it, dest); return;      // LQD
    case 0x37: ReadVi(it); WriteVi(it); ReadVf(is, dest); return;      // SQD
    case 0x38: ReadVf(is, fsf); ReadVf(it, ftf); setQ(7); return;   // DIV
    case 0x39: ReadVf(it, ftf); setQ(7); return;                       // SQRT
    case 0x3A: ReadVf(is, fsf); ReadVf(it, ftf); setQ(13); return;     // RSQRT
    case 0x3B: return;                                                 // WAITQ
    case 0x3C: ReadVf(is, fsf); WriteVi(it); return;                   // MTIR
    case 0x3D: ReadVi(is); WriteVf(it, dest); return;                  // MFIR
    case 0x3E: ReadVi(is); WriteVi(it); return;                        // ILWR
    case 0x3F: ReadVi(is); ReadVi(it); return;                         // ISWR
    case 0x64: ReadTimed(kP, g_now.pAt, cycle); WriteVf(it, dest); return;   // MFP
    case 0x68: case 0x69: WriteVi(it); return;                         // XTOP, XITOP
    case 0x6C: ReadVi(is); return;                                     // XGKICK
    case 0x7B: return;                                                 // WAITP
    case 0x70: case 0x71: case 0x72: case 0x73: ReadVf(is, 0xE); setP(11); return;
    case 0x74: ReadVf(is, 0xC); setP(54); return;
    case 0x75: ReadVf(is, 0xA); setP(54); return;
    case 0x76: ReadVf(is, 0xF); setP(12); return;
    default: ReadVf(is, fsf); setP(12); return;                        // the one-field EFU operations
    }
}

}  // namespace

bool Enabled()
{
    static const bool on = std::getenv("SLY1_VU1_LIVE") != nullptr;
    return on;
}

void Begin(std::uint32_t entry, std::uint32_t chunk)
{
    if (g_running)   // the last program's writes: it is their writer now
        for (int b = 0; b < kCount; b++)
            if (g_now.written[b])
                g_lastWriter[b] = {g_chunk, g_entry};
    g_now = Program{};
    g_now.qAt = g_now.pAt = g_now.macAt = g_now.statusAt = g_now.clipAt = kNever;
    g_entry = entry;
    g_chunk = chunk;
    g_historyAt = 0;
    g_running = true;
}

void Instruction(std::uint32_t pc, std::uint32_t hi, std::uint32_t lo, std::uint64_t cycle)
{
    if (!g_running)
        return;
    g_pc = pc;
    g_history[g_historyAt++ & 63] = pc;
    if (!(hi & 0x80000000u)) {
        int k = core::Runner::Stalls(lo);
        if (k == 1 && g_now.qAt != kNever && g_now.qAt > cycle)
            cycle = g_now.qAt;
        if (k == 2 && g_now.pAt != kNever && g_now.pAt > cycle)
            cycle = g_now.pAt;
    }
    // the timed pieces whose first write has landed are the program's own from here
    if (g_now.qAt <= cycle) Write(kQ);
    if (g_now.pAt <= cycle) Write(kP);
    if (g_now.macAt <= cycle) Write(kMac);
    if (g_now.statusAt <= cycle) Write(kStatus);
    if (g_now.clipAt <= cycle) Write(kClip);
    // the lower operation reads first in the same instruction as the upper; both read the
    // registers as they were before it, so the reads of both go before either's writes
    if (hi & 0x80000000u) {
        Upper(hi, cycle);
        Write(kI);
    } else {
        Set saved = g_now.written;
        Upper(hi, cycle);
        Set upperWrites = g_now.written & ~saved;
        g_now.written = saved;
        Lower(lo, cycle);
        g_now.written |= upperWrites;
    }
}

const Set& Inputs() { return g_union; }

Set Parse(const char* names)
{
    Set out;
    std::string all(names ? names : "");
    std::size_t at = 0;
    while (at < all.size()) {
        std::size_t endAt = all.find(' ', at);
        if (endAt == std::string::npos)
            endAt = all.size();
        std::string w = all.substr(at, endAt - at);
        at = endAt + 1;
        if (w.empty())
            continue;
        int r = 0;
        char fields[8] = {0};
        if (std::sscanf(w.c_str(), "vf%d.%7s", &r, fields) == 2 && r >= 0 && r < 32) {
            for (const char* f = fields; *f; f++) {
                const char* k = std::strchr("xyzw", *f);
                if (!k) {
                    std::fprintf(stderr, "port: vu1 live: bad name %s\n", w.c_str());
                    std::abort();
                }
                out[kVf + r * 4 + (int)(k - "xyzw")] = true;
            }
            continue;
        }
        if (std::sscanf(w.c_str(), "vi%d", &r) == 1 && r > 0 && r < 16) {
            out[kVi + r] = true;
            continue;
        }
        if (w.compare(0, 4, "ACC.") == 0) {
            for (const char* f = w.c_str() + 4; *f; f++)
                out[kAcc + (int)(std::strchr("xyzw", *f) - "xyzw")] = true;
            continue;
        }
        static const char* const kNames[] = {"Q", "P", "I", "MAC", "status", "sticky", "clip"};
        bool found = false;
        for (int k = 0; k < 7; k++)
            if (w == kNames[k]) {
                out[kQ + k] = true;
                found = true;
            }
        if (!found) {
            std::fprintf(stderr, "port: vu1 live: bad name %s\n", w.c_str());
            std::abort();
        }
    }
    return out;
}

const char* Name(int bit, char* buf, int size)
{
    if (bit < kVi)
        std::snprintf(buf, size, "vf%02d.%c", bit / 4, "xyzw"[bit % 4]);
    else if (bit < kAcc)
        std::snprintf(buf, size, "vi%02d", bit - kVi);
    else if (bit < kQ)
        std::snprintf(buf, size, "ACC.%c", "xyzw"[bit - kAcc]);
    else {
        static const char* const kNames[] = {"Q", "P", "I", "MAC", "status", "sticky", "clip"};
        std::snprintf(buf, size, "%s", kNames[bit - kQ]);
    }
    return buf;
}

void Report()
{
    char name[16];
    auto print = [&](const Set& s) {
        // runs of a register's fields together: vf12.xyzw
        for (int b = 0; b < kCount; b++) {
            if (!s[b])
                continue;
            if (b < kVi) {
                int r = b / 4;
                std::fprintf(stderr, " vf%02d.", r);
                for (int k = 0; k < 4; k++)
                    if (s[r * 4 + k])
                        std::fputc("xyzw"[k], stderr);
                b = r * 4 + 3;
            } else {
                std::fprintf(stderr, " %s", Name(b, name, sizeof name));
            }
        }
        std::fputc('\n', stderr);
    };
    for (const auto& e : g_byEntry) {
        std::fprintf(stderr, "vu1 live: chunk %08X entry 0x%03X reads:", e.first.first, e.first.second);
        print(e.second);
        std::fprintf(stderr, "vu1 live:     first read at");
        for (const auto& w : g_where[e.first])
            std::fprintf(stderr, " %s@%03X", Name(w.first, name, sizeof name), w.second);
        std::fputc('\n', stderr);
    }
    std::fprintf(stderr, "vu1 live: all entries read:");
    print(g_union);
    for (const auto& w : g_leaves) {
        std::fprintf(stderr, "vu1 live: chunk %08X entry 0x%03X leaves:", w.first.first, w.first.second);
        for (const auto& b : w.second)
            std::fprintf(stderr, " %s (for %08X 0x%03X)", Name(b.first, name, sizeof name), b.second.first,
                         b.second.second);
        std::fputc('\n', stderr);
    }
}

}  // namespace live
}  // namespace vu1
}  // namespace port
