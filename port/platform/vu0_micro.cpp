// SPDX-License-Identifier: AGPL-3.0-or-later
// VU0 in micro mode (port/vu0_micro.h): an interpreter for the instructions the game's VU0
// microcode uses, written from the public descriptions of the VU's instruction formats and
// operations. Each 64-bit instruction is an upper (FMAC) and a lower operation, both reading the
// registers as they were before it; arithmetic follows VU0's float rules (port/vu0_impl.h:
// no infinity or NaN, denormals as zero). Branches have a delay slot; an instruction with the
// end bit is followed by one more. The MAC flags an FMAC operation sets become visible four
// instructions later (the pipeline's latency), which the game's code is scheduled around: FMEQ
// reads them as they stood then. An instruction the game's microcode does not use stops the run
// with its encoding.
#include "port/vu0_micro.h"

#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "port/vu0_impl.h"

namespace port {
namespace vu0 {

namespace {

State g_state;

using port_vu0::Fix;

// MAC flags of one result field: zero (bit 0 + n) and sign (bit 4 + n), n = 3 for x .. 0 for w.
std::uint32_t FieldFlags(float r, int field)
{
    int n = 3 - field;
    std::uint32_t f = 0;
    if (r == 0.0f)
        f |= 1u << n;
    if (std::signbit(r) && r != 0.0f)
        f |= 1u << (4 + n);
    return f;
}

[[noreturn]] void Unknown(const char* which, std::uint32_t pc, std::uint32_t w)
{
    std::fprintf(stderr, "port: VU0 %s instruction %08X at 0x%03X is not interpreted\n", which, w, pc * 8);
    std::abort();
}

struct Runner {
    State& s = g_state;
    std::uint64_t cycle = 0;
    // MAC flags by the cycle they become visible
    struct Pending { std::uint64_t at; std::uint32_t mac; } pending[8] = {};
    int npending = 0;

    std::uint32_t MacNow()
    {
        std::uint32_t mac = s.mac;
        int keep = 0;
        for (int k = 0; k < npending; k++) {
            if (pending[k].at <= cycle)
                mac = s.mac = pending[k].mac;
            else
                pending[keep++] = pending[k];
        }
        npending = keep;
        return mac;
    }
    void SetMac(std::uint32_t mac)
    {
        if (npending == 8) {
            s.mac = pending[0].mac;
            std::memmove(pending, pending + 1, sizeof pending[0] * 7);
            npending = 7;
        }
        pending[npending++] = {cycle + 4, mac};
    }

    // --- upper

    struct Out {
        int reg = -1;          // vf written, or -1; 32 for ACC
        float v[4];
        int dest = 0;          // field mask: bit 3 x .. bit 0 w
        bool flags = false;    // an arithmetic result: sets MAC
    };

    static int Dest(std::uint32_t w) { return (w >> 21) & 0xF; }   // x = 8, y = 4, z = 2, w = 1
    static bool Has(int dest, int field) { return dest & (8 >> field); }

    Out Upper(std::uint32_t pc, std::uint32_t w)
    {
        Out o;
        int op = w & 0x3F, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31;
        int dest = Dest(w);
        const float* a = s.vf[fs];
        const float* b = s.vf[ft];
        auto lanes = [&](auto f) {
            for (int k = 0; k < 4; k++)
                if (Has(dest, k))
                    o.v[k] = f(k);
        };
        enum Kind { Add, Sub, Madd, Msub, Max, Mini, Mul };
        auto arith = [&](Kind kind, auto other, bool toAcc) {
            o.reg = toAcc ? 32 : fd;
            o.dest = dest;
            o.flags = kind != Max && kind != Mini;
            lanes([&](int k) -> float {
                float x = a[k], y = other(k);
                switch (kind) {
                case Add: return port_vu0::Add(x, y);
                case Sub: return port_vu0::Sub(x, y);
                case Mul: return port_vu0::Mul(x, y);
                case Madd: return port_vu0::Add(s.acc[k], port_vu0::Mul(x, y));
                case Msub: return port_vu0::Sub(s.acc[k], port_vu0::Mul(x, y));
                case Max: return x > y ? x : y;
                default: return x < y ? x : y;
                }
            });
        };
        static const Kind bcKinds[] = {Add, Sub, Madd, Msub, Max, Mini, Mul};
        if (op < 0x1C) {
            int bc = op & 3;
            arith(bcKinds[op >> 2], [&](int) { return b[bc]; }, false);
            return o;
        }
        switch (op) {
        case 0x1C: arith(Mul, [&](int) { return s.q; }, false); return o;
        case 0x1E: arith(Mul, [&](int) { return s.i; }, false); return o;
        case 0x20: arith(Add, [&](int) { return s.q; }, false); return o;
        case 0x22: arith(Add, [&](int) { return s.i; }, false); return o;
        case 0x24: arith(Sub, [&](int) { return s.q; }, false); return o;
        case 0x26: arith(Sub, [&](int) { return s.i; }, false); return o;
        case 0x28: arith(Add, [&](int k) { return b[k]; }, false); return o;
        case 0x29: arith(Madd, [&](int k) { return b[k]; }, false); return o;
        case 0x2A: arith(Mul, [&](int k) { return b[k]; }, false); return o;
        case 0x2B: arith(Max, [&](int k) { return b[k]; }, false); return o;
        case 0x2C: arith(Sub, [&](int k) { return b[k]; }, false); return o;
        case 0x2D: arith(Msub, [&](int k) { return b[k]; }, false); return o;
        case 0x2F: arith(Mini, [&](int k) { return b[k]; }, false); return o;
        }
        if (op >= 0x3C) {
            int o2 = (((w >> 6) & 31) << 2) | (w & 3);
            if (o2 < 0x10) {
                int bc = o2 & 3;
                static const Kind accKinds[] = {Add, Sub, Madd, Msub};
                arith(accKinds[o2 >> 2], [&](int) { return b[bc]; }, true);
                return o;
            }
            if (o2 >= 0x18 && o2 <= 0x1B) {
                int bc = o2 & 3;
                arith(Mul, [&](int) { return b[bc]; }, true);
                return o;
            }
            switch (o2) {
            case 0x1D:   // ABS
                o.reg = ft;
                o.dest = dest;
                lanes([&](int k) { return std::fabs(a[k]); });
                return o;
            case 0x28: arith(Add, [&](int k) { return b[k]; }, true); return o;
            case 0x29: arith(Madd, [&](int k) { return b[k]; }, true); return o;
            case 0x2A: arith(Mul, [&](int k) { return b[k]; }, true); return o;
            case 0x2F: return o;   // NOP
            }
        }
        Unknown("upper", pc, w);
    }

    void Write(const Out& o)
    {
        if (o.reg < 0)
            return;
        float* r = o.reg == 32 ? s.acc : s.vf[o.reg];
        std::uint32_t mac = 0;
        for (int k = 0; k < 4; k++) {
            if (!Has(o.dest, k))
                continue;
            if (o.reg != 0)
                r[k] = o.v[k];
            if (o.flags)
                mac |= FieldFlags(o.v[k], k);
        }
        if (o.flags)
            SetMac(mac);
    }

    // --- lower

    static int S11(std::uint32_t w) { int v = w & 0x7FF; return v & 0x400 ? v - 0x800 : v; }

    float* Qword(std::uint32_t qaddr) { return reinterpret_cast<float*>(s.data + (qaddr & 0xFF) * 16); }

    // Runs the lower operation; gives the branch target (in instructions) or -1.
    long Lower(std::uint32_t pc, std::uint32_t w, Out& vfWrite)
    {
        int op = (w >> 25) & 0x7F, it = (w >> 16) & 31, is = (w >> 11) & 31, id = (w >> 6) & 31;
        int dest = Dest(w);
        auto viw = [&](int r, std::uint32_t v) {
            if (r & 15)
                s.vi[r & 15] = (std::uint16_t)v;
        };
        auto vi = [&](int r) -> std::uint16_t { return s.vi[r & 15]; };
        auto load = [&](int reg, std::uint32_t qaddr) {
            vfWrite.reg = reg;
            vfWrite.dest = dest;
            std::memcpy(vfWrite.v, Qword(qaddr), 16);
        };
        auto store = [&](int reg, std::uint32_t qaddr) {
            float* m = Qword(qaddr);
            for (int k = 0; k < 4; k++)
                if (Has(dest, k))
                    m[k] = s.vf[reg][k];
        };
        auto target = [&]() -> long { return (long)pc + 1 + S11(w); };
        switch (op) {
        case 0x00: load(it, vi(is) + S11(w)); return -1;   // LQ
        case 0x01: store(is, vi(it) + S11(w)); return -1;  // SQ
        case 0x04: {                                        // ILW
            float* m = Qword(vi(is) + S11(w));
            for (int k = 0; k < 4; k++)
                if (Has(dest, k)) {
                    std::uint32_t b;
                    std::memcpy(&b, &m[k], 4);
                    viw(it, b);
                    break;
                }
            return -1;
        }
        case 0x05: {                                        // ISW
            float* m = Qword(vi(is) + S11(w));
            for (int k = 0; k < 4; k++)
                if (Has(dest, k)) {
                    std::uint32_t b = vi(it);
                    std::memcpy(&m[k], &b, 4);
                }
            return -1;
        }
        case 0x08: viw(it, vi(is) + (((w >> 10) & 0x7800) | (w & 0x7FF))); return -1;   // IADDIU
        case 0x09: viw(it, vi(is) - (((w >> 10) & 0x7800) | (w & 0x7FF))); return -1;   // ISUBIU
        case 0x18: viw(it, MacNow() == vi(is) ? 1 : 0); return -1;                       // FMEQ
        case 0x1A: viw(it, MacNow() & vi(is)); return -1;                                // FMAND
        case 0x1B: viw(it, (MacNow() | vi(is)) & 0xFFFF); return -1;                     // FMOR
        case 0x20: return target();                                                      // B
        case 0x21: viw(it, pc + 2); return target();                                     // BAL
        case 0x24: return vi(is);                                                        // JR
        case 0x25: { long t = vi(is); viw(it, pc + 2); return t; }                       // JALR
        case 0x28: return vi(it) == vi(is) ? target() : -1;                              // IBEQ
        case 0x29: return vi(it) != vi(is) ? target() : -1;                              // IBNE
        case 0x2C: return (std::int16_t)vi(is) < 0 ? target() : -1;                      // IBLTZ
        case 0x2D: return (std::int16_t)vi(is) > 0 ? target() : -1;                      // IBGTZ
        case 0x2E: return (std::int16_t)vi(is) <= 0 ? target() : -1;                     // IBLEZ
        case 0x2F: return (std::int16_t)vi(is) >= 0 ? target() : -1;                     // IBGEZ
        case 0x40: break;
        default: Unknown("lower", pc, w);
        }
        int f = w & 0x3F;
        switch (f) {
        case 0x30: viw(id, vi(is) + vi(it)); return -1;   // IADD
        case 0x31: viw(id, vi(is) - vi(it)); return -1;   // ISUB
        case 0x32: {                                      // IADDI
            int im = (w >> 6) & 31;
            viw(it, vi(is) + (im & 16 ? im - 32 : im));
            return -1;
        }
        case 0x34: viw(id, vi(is) & vi(it)); return -1;   // IAND
        case 0x35: viw(id, vi(is) | vi(it)); return -1;   // IOR
        }
        if (f < 0x3C)
            Unknown("lower", pc, w);
        int o2 = (((w >> 6) & 31) << 2) | (w & 3);
        switch (o2) {
        case 0x30:   // MOVE (and the lower NOP, MOVE to vf0)
            vfWrite.reg = it;
            vfWrite.dest = dest;
            std::memcpy(vfWrite.v, s.vf[is], 16);
            return -1;
        case 0x31:   // MR32: rotate the fields (x gets y, y z, z w, w x)
            vfWrite.reg = it;
            vfWrite.dest = dest;
            for (int k = 0; k < 4; k++)
                vfWrite.v[k] = s.vf[is][(k + 1) & 3];
            return -1;
        case 0x34: load(it, vi(is)); viw(is, vi(is) + 1); return -1;          // LQI
        case 0x35: store(is, vi(it)); viw(it, vi(it) + 1); return -1;         // SQI
        case 0x36: viw(is, vi(is) - 1); load(it, vi(is)); return -1;          // LQD
        case 0x37: viw(it, vi(it) - 1); store(is, vi(it)); return -1;         // SQD
        case 0x38: {                                                          // DIV
            float n = s.vf[is][(w >> 21) & 3], d = s.vf[it][(w >> 23) & 3];
            s.q = d == 0.0f ? Fix(std::copysign(3.4028235e38f, n) * (std::signbit(d) ? -1.0f : 1.0f))
                            : Fix(n / d);
            return -1;
        }
        case 0x39: s.q = Fix(std::sqrt(std::fabs(s.vf[it][(w >> 23) & 3]))); return -1;   // SQRT
        case 0x3A: {                                                                      // RSQRT
            float n = s.vf[is][(w >> 21) & 3], d = std::sqrt(std::fabs(s.vf[it][(w >> 23) & 3]));
            s.q = d == 0.0f ? Fix(std::copysign(3.4028235e38f, n)) : Fix(n / d);
            return -1;
        }
        case 0x3B: return -1;                                                  // WAITQ
        case 0x3C: {                                                           // MTIR
            std::uint32_t b;
            std::memcpy(&b, &s.vf[is][(w >> 21) & 3], 4);
            viw(it, b);
            return -1;
        }
        case 0x3D:                                                             // MFIR
            vfWrite.reg = it;
            vfWrite.dest = dest;
            for (int k = 0; k < 4; k++) {
                std::int32_t v = (std::int16_t)vi(is);
                std::memcpy(&vfWrite.v[k], &v, 4);
            }
            return -1;
        }
        Unknown("lower", pc, w);
    }
};

}  // namespace

State& Vu0() { return g_state; }

namespace {

// The particle filter (0x010 to 0x058), as the game's microcode has it: ten instructions, run once
// for each particle (port/include: Vu0::ParticleFilter). Interpreted it was a quarter of the game's
// main thread on the Switch in A Stealthy Approach (the rain), so it is done here in the host's own
// code, with the interpreter's arithmetic, field by field in its order:
//   ACC.xyz = vf31 * vf24.x + vf30 * vf24.y + vf29 * vf24.z;  vf22.xyz = ACC + vf28 * vf00.w
//   vf21.xyz = vf22 * vf27.x + vf23;  vf26.xyz = min(vf26, vf21);  vf25.xyz = max(vf25, vf21)
//   vf22.w = vf24.w;  vf21.w = vf23.w
// Only while the microcode there is that program, word for word. SLY1_VU0_NATIVE=0: interpreted;
// SLY1_VU0_CHECK=1: both, compared (every register, ACC, the flags, where it stops).
const std::uint32_t kParticleFilter[20] = {
    0x8000033C, 0x01D8F9BC, 0x8000033C, 0x01D8F0BD, 0x8000033C, 0x01D8E8BE, 0x8000033C, 0x01C0E58B, 0x8000033C, 0x01DBB558,
    0x8000033C, 0x01D7AD68, 0x8036C33C, 0x01D5D6AF, 0x8035BB3C, 0x01D5CE6B, 0x8000033C, 0x400002FF, 0x8000033C, 0x000002FF,
};

void ParticleFilterNative(State& s)
{
    s.vf[0][0] = s.vf[0][1] = s.vf[0][2] = 0.0f;
    s.vf[0][3] = 1.0f;
    s.vi[0] = 0;
    const float ex = s.vf[24][0], ey = s.vf[24][1], ez = s.vf[24][2], scale = s.vf[27][0];
    std::uint32_t mac = 0;
    for (int k = 0; k < 3; k++) {
        float acc = port_vu0::Mul(s.vf[31][k], ex);
        acc = port_vu0::Add(acc, port_vu0::Mul(s.vf[30][k], ey));
        acc = port_vu0::Add(acc, port_vu0::Mul(s.vf[29][k], ez));
        s.acc[k] = acc;
        const float moved = port_vu0::Add(acc, port_vu0::Mul(s.vf[28][k], 1.0f));
        const float at = port_vu0::Add(port_vu0::Mul(moved, scale), s.vf[23][k]);
        s.vf[22][k] = moved;
        s.vf[21][k] = at;
        mac |= FieldFlags(at, k);
        const float lo = s.vf[26][k], hi = s.vf[25][k];
        s.vf[26][k] = lo < at ? lo : at;
        s.vf[25][k] = hi > at ? hi : at;
    }
    s.vf[22][3] = s.vf[24][3];
    s.vf[21][3] = s.vf[23][3];
    s.mac = mac;
    s.pc = 12;
}

// The first light test (0x060 to 0x160), which the game runs once for each group of lights
// (Vu0::NextLightGroup): from the record at vi3 on, four records at a time, each record plus the
// sphere in vf31, squared, x + y + z - w, and whether that is below zero (the MAC flags four
// instructions on, against vi8 = 0x80) kept in vi11 to vi14; the loop's lower half meanwhile
// shifts the four results of the turn before into vi2, one for each of vi4's count, and leaves
// when the count is out. Written out a turn of the microcode at a time, each register written
// where the microcode writes it, so that what it leaves (the mask, the records read ahead, the
// half-made sums of a turn it left) is the interpreter's to the bit.
const std::uint32_t kLightTest[66] = {
    0x10080080, 0x000002FF, 0x81E11B7C, 0x000002FF, 0x81E21B7C, 0x000002FF, 0x81E31B7C, 0x000002FF,
    0x81E41B7C, 0x000002FF, 0x10042004, 0x000002FF, 0x800427F2, 0x01FF0AE8, 0x800210B0, 0x01FF1328,
    0x5C002016, 0x01FF1B68, 0x800B10B0, 0x01FF23A8, 0x800427F2, 0x01EB5AEA, 0x800210B0, 0x01EC632A,
    0x5C002012, 0x01ED6B6A, 0x800C10B0, 0x01EE73AA, 0x800427F2, 0x010B5AC1, 0x800210B0, 0x010C6301,
    0x5C00200E, 0x010D6B41, 0x800D10B0, 0x010E7381, 0x800427F2, 0x010B5AC2, 0x800210B0, 0x010C6302,
    0x5C00200A, 0x010D6B42, 0x800E10B0, 0x010E7382, 0x81E11B7C, 0x010B5AC7, 0x81E21B7C, 0x010C6307,
    0x81E31B7C, 0x010D6B47, 0x81E41B7C, 0x010E7387, 0x300B4000, 0x000002FF, 0x300C4000, 0x000002FF,
    0x300D4000, 0x000002FF, 0x400007E8, 0x000002FF, 0x300E4000, 0x000002FF, 0x8000033C, 0x400002FF,
    0x8000033C, 0x000002FF,
};

void LightTestNative(State& s)
{
    s.vf[0][0] = s.vf[0][1] = s.vf[0][2] = 0.0f;
    s.vf[0][3] = 1.0f;
    std::uint16_t* vi = s.vi;
    vi[0] = 0;
    vi[8] = 0x80;
    const auto load = [&](int reg) {
        std::memcpy(s.vf[reg], s.data + (vi[3] & 0xFF) * 16, 16);
        vi[3] = (std::uint16_t)(vi[3] + 1);
    };
    for (int r = 1; r <= 4; r++)
        load(r);
    vi[4] = (std::uint16_t)(vi[4] + 4);
    std::uint32_t last = s.mac;   // the flags of the last arithmetic, as they settle at the end
    // one upper instruction of block b (0 the add, 1 the square, 2 and 3 the sum, 4 the w taken off) on vf(11 + r)
    const auto upper = [&](int b, int r) {
        float* v = s.vf[11 + r];
        std::uint32_t mac = 0;
        if (b == 0) {
            for (int k = 0; k < 4; k++) {
                v[k] = port_vu0::Add(s.vf[1 + r][k], s.vf[31][k]);
                mac |= FieldFlags(v[k], k);
            }
        } else if (b == 1) {
            for (int k = 0; k < 4; k++) {
                v[k] = port_vu0::Mul(v[k], v[k]);
                mac |= FieldFlags(v[k], k);
            }
        } else {
            v[0] = b == 4 ? port_vu0::Sub(v[0], v[3]) : port_vu0::Add(v[0], v[b - 1]);
            mac = FieldFlags(v[0], 0);
        }
        last = mac;
        return mac;
    };
    for (int guard = 0; guard < 1 << 16; guard++) {
        for (int b = 0; b < 4; b++) {
            upper(b, 0);
            vi[4] = (std::uint16_t)(vi[4] - 1);
            upper(b, 1);
            vi[2] = (std::uint16_t)(vi[2] + vi[2]);
            upper(b, 2);
            const bool out = (std::int16_t)vi[4] <= 0;
            upper(b, 3);
            vi[2] = (std::uint16_t)(vi[2] + vi[11 + b]);
            if (out) {
                s.mac = last;
                s.pc = 45;
                return;
            }
        }
        std::uint32_t mac[4];
        for (int r = 0; r < 4; r++) {
            mac[r] = upper(4, r);
            load(1 + r);
        }
        for (int r = 0; r < 4; r++)
            vi[11 + r] = mac[r] == vi[8] ? 1 : 0;
    }
    std::fprintf(stderr, "port: VU0's light test ran without ending\n");
    std::abort();
}

void Interpret(std::uint32_t addr);

}  // namespace

void Run(std::uint32_t addr)
{
    static const bool native = !(std::getenv("SLY1_VU0_NATIVE") && std::getenv("SLY1_VU0_NATIVE")[0] == '0');
    static const bool check = std::getenv("SLY1_VU0_CHECK") != nullptr;
    // SLY1_VU0_NATIVE=ab: native and interpreted in turn, ten seconds each, said as it changes
    // (to compare the two in one played run)
    static const bool turns = std::getenv("SLY1_VU0_NATIVE") && std::getenv("SLY1_VU0_NATIVE")[0] == 'a';
    if (turns && addr == 0x10) {
        static int was = -1;
        const int now = (int)(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 10 % 2);
        if (now != was)
            std::fprintf(stderr, "vu0ab: the particle filter %s\n", now ? "native" : "interpreted");
        was = now;
        if (!now) {
            Interpret(addr);
            return;
        }
    }
    if (addr == 0x10 && native && std::memcmp(&g_state.micro[4], kParticleFilter, sizeof kParticleFilter) == 0) {
        if (!check) {
            ParticleFilterNative(g_state);
            return;
        }
        static State before;
        std::memcpy(&before, &g_state, offsetof(State, data));
        Interpret(addr);
        State& s = g_state;
        static State mine;
        std::memcpy(&mine, &before, offsetof(State, data));
        ParticleFilterNative(mine);
        static unsigned long runs, wrong;
        runs++;
        if (std::memcmp(mine.vf, s.vf, sizeof s.vf) != 0 || std::memcmp(mine.acc, s.acc, 12) != 0 || mine.mac != s.mac || mine.pc != s.pc) {
            if (wrong++ < 8)
                std::fprintf(stderr, "vu0check: the particle filter differs (run %lu): mac %X against %X, pc %u against %u\n", runs, mine.mac, s.mac, mine.pc, s.pc);
        }
        if (runs % 100000 == 0)
            std::fprintf(stderr, "vu0check: %lu runs of the particle filter, %lu differed\n", runs, wrong);
        return;
    }
    if (addr == 0x60 && native && std::memcmp(&g_state.micro[24], kLightTest, sizeof kLightTest) == 0) {
        if (!check) {
            LightTestNative(g_state);
            return;
        }
        static State before, mine;
        std::memcpy(&before, &g_state, offsetof(State, micro));
        Interpret(addr);
        std::memcpy(&mine, &before, offsetof(State, micro));
        LightTestNative(mine);
        static unsigned long runs, wrong;
        runs++;
        if (std::memcmp(&mine, &g_state, offsetof(State, data)) != 0) {
            if (wrong++ < 8) {
                std::fprintf(stderr, "vu0check: the light test differs (run %lu): mac %X against %X, pc %u against %u; vi", runs, mine.mac, g_state.mac, mine.pc, g_state.pc);
                for (int k = 0; k < 16; k++)
                    std::fprintf(stderr, " %X/%X", mine.vi[k], g_state.vi[k]);
                std::fprintf(stderr, "; vf other:");
                for (int k = 0; k < 32; k++)
                    if (std::memcmp(mine.vf[k], g_state.vf[k], 16) != 0)
                        std::fprintf(stderr, " %d", k);
                std::fprintf(stderr, "\n");
            }
        }
        if (runs % 20000 == 0)
            std::fprintf(stderr, "vu0check: %lu runs of the light test, %lu differed\n", runs, wrong);
        return;
    }
    Interpret(addr);
}

namespace {

void Interpret(std::uint32_t addr)
{
    State& s = g_state;
    s.vf[0][0] = s.vf[0][1] = s.vf[0][2] = 0.0f;
    s.vf[0][3] = 1.0f;
    s.vi[0] = 0;
    {   // SLY1_VU0_DUMP=1: each microprogram's instructions, the first time it is run
        static const bool dump = std::getenv("SLY1_VU0_DUMP") != nullptr;
        static std::uint32_t seen[8];
        static int nseen;
        bool had = false;
        for (int k = 0; k < nseen; k++)
            had = had || seen[k] == addr;
        if (dump && !had && nseen < 8) {
            seen[nseen++] = addr;
            for (std::uint32_t at = (addr / 8) & 511, n = 0; n < 96; n++, at = (at + 1) & 511)
                std::fprintf(stderr, "vu0dump: %03X %03X: %08X %08X\n", addr, at * 8, s.micro[at * 2 + 1], s.micro[at * 2]);
        }
    }
    Runner r;
    std::uint32_t pc = (addr / 8) & 511;
    long branch = -1;     // a taken branch's target, after the delay slot
    bool ending = false;  // the end bit was seen: one more instruction
    for (int guard = 0; guard < 1 << 20; guard++) {
        std::uint32_t lo = s.micro[pc * 2], hi = s.micro[pc * 2 + 1];
        Runner::Out up = r.Upper(pc, hi);
        Runner::Out low;
        long next = -1;
        if (hi & 0x80000000u) {
            std::memcpy(&s.i, &lo, 4);   // the I bit: the lower word is a float for I
        } else {
            next = r.Lower(pc, lo, low);
        }
        // Both read the old registers; the lower's register write, then the upper's (which wins).
        r.Write(low);
        low.flags = false;
        r.Write(up);
        r.cycle++;
        bool end = ending;
        ending = (hi & 0x40000000u) != 0;
        long to = branch >= 0 ? branch : (long)pc + 1;
        branch = next;
        pc = (std::uint32_t)to & 511;
        if (end) {
            s.pc = pc;
            for (int k = 0; k < r.npending; k++)   // the flags of the last operations settle
                s.mac = r.pending[k].mac;
            return;
        }
    }
    std::fprintf(stderr, "port: a VU0 microprogram from 0x%03X ran without ending\n", addr);
    std::abort();
}

}  // namespace

}  // namespace vu0
}  // namespace port
