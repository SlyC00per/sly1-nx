// SPDX-License-Identifier: AGPL-3.0-or-later
// VU1's execution core (port/vu1.h), shared by the interpreter (port/render/vu1.cpp) and the
// microcode translated to C++ at build time (tools/vu1_translate.py): one instruction's upper
// and lower operations, their register writes, the flags and Q and P with their latencies. The
// rules are described in port/render/vu1.cpp.
#ifndef PORT_RENDER_VU1_RUNNER_H
#define PORT_RENDER_VU1_RUNNER_H

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "port/ptr32.h"
#include "port/vu0_impl.h"
#include "port/vu1.h"

namespace port {
namespace vu1 {
namespace core {


using port_vu0::Fix;

// An operand as the VU reads it: the largest exponent is a finite number, a denormal 0.
inline float In(float x)
{
    std::uint32_t b;
    std::memcpy(&b, &x, 4);
    std::uint32_t e = (b >> 23) & 0xFF;
    if (e == 0xFF) {
        b = (b & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&x, &b, 4);
    } else if (e == 0) {
        b &= 0x80000000u;
        std::memcpy(&x, &b, 4);
    }
    return x;
}

inline float Add(float a, float b) { return Fix(In(a) + In(b)); }
inline float Sub(float a, float b) { return Fix(In(a) - In(b)); }
inline float Mul(float a, float b) { return Fix(In(a) * In(b)); }

inline std::uint32_t Bits(float f)
{
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

inline float FromBits(std::uint32_t b)
{
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

[[noreturn]] inline void Unknown(const char* which, std::uint32_t pc, std::uint32_t w)
{
    std::fprintf(stderr, "port: VU1 %s instruction %08X at 0x%03X is not interpreted\n", which, w, pc);
    std::abort();
}

struct Runner {
    State& s = Vu1();
    const Context& ctx;
    std::uint8_t* data;
    std::uint64_t cycle = 0;

    explicit Runner(const Context& c) : ctx(c), data(vu1::Data()) {}

    // Between instructions: a taken branch's target still to come after its delay slot, the end
    // bit's one more instruction, the program's end; and the instructions run.
    long branch = -1;
    bool ending = false;
    bool done = false;
    std::uint64_t executed = 0;

    // --- the flags, seen four instructions after the operation that sets them. An instruction
    // sets MAC and status at most once and the clip flags at most once, so each is a ring of four
    // by the cycle it was set in: an entry is due when the ring comes back to its slot.

    // Each a queue in the order set (at most four are pending: one per instruction, due four
    // instructions later).
    struct FlagEntry { std::uint64_t at; std::uint32_t mac, status; };
    struct ClipEntry { std::uint64_t at; std::uint32_t clip; };
    FlagEntry flagQueue[4];
    ClipEntry clipQueue[4];
    unsigned flagHead = 0, flagCount = 0, clipHead = 0, clipCount = 0;
    std::uint32_t latestClip = 0;   // the newest clip flags, seen or not (CLIP shifts them)
    bool latestClipSet = false;

    void Commit(const FlagEntry& e)
    {
        s.mac = e.mac;
        s.status = (s.status & 0xFF0) | e.status | ((e.status & 0xF) << 6);
    }
    __attribute__((always_inline)) void Settle()
    {
        while (flagCount && flagQueue[flagHead].at <= cycle) {
            Commit(flagQueue[flagHead]);
            flagHead = (flagHead + 1) & 3;
            flagCount--;
        }
        while (clipCount && clipQueue[clipHead].at <= cycle) {
            s.clip = clipQueue[clipHead].clip;
            clipHead = (clipHead + 1) & 3;
            clipCount--;
        }
    }
    __attribute__((always_inline)) void PushFlags(std::uint32_t mac, std::uint32_t status)
    {
        if (flagCount == 4) {   // the oldest is due: set four instructions ago
            Commit(flagQueue[flagHead]);
            flagHead = (flagHead + 1) & 3;
            flagCount--;
        }
        flagQueue[(flagHead + flagCount) & 3] = {cycle + 4, mac, status};
        flagCount++;
    }
    __attribute__((always_inline)) void PushClip(std::uint32_t clip)
    {
        if (clipCount == 4) {
            s.clip = clipQueue[clipHead].clip;
            clipHead = (clipHead + 1) & 3;
            clipCount--;
        }
        clipQueue[(clipHead + clipCount) & 3] = {cycle + 4, clip};
        clipCount++;
        latestClip = clip;
        latestClipSet = true;
    }
    std::uint32_t Mac() { Settle(); return s.mac & 0xFFFF; }
    std::uint32_t Status() { Settle(); return s.status & 0xFFF; }
    std::uint32_t Clip() { Settle(); return s.clip & 0xFFFFFF; }
    std::uint32_t LatestClip() { return latestClipSet ? latestClip : s.clip; }

    // --- Q and P, written after a latency

    struct Late { std::uint64_t at = 0; float v = 0; bool busy = false; };
    Late lateQ, lateP;
    float Q()
    {
        if (lateQ.busy && lateQ.at <= cycle) {
            s.q = lateQ.v;
            lateQ.busy = false;
        }
        return s.q;
    }
    float P()
    {
        if (lateP.busy && lateP.at <= cycle) {
            s.p = lateP.v;
            lateP.busy = false;
        }
        return s.p;
    }
    void WaitQ() { if (lateQ.busy && lateQ.at > cycle) cycle = lateQ.at; Q(); }
    void WaitP() { if (lateP.busy && lateP.at > cycle) cycle = lateP.at; P(); }
    // A lower operation that waits for the divider (WAITQ, or DIV, SQRT, RSQRT while one is
    // busy) or the EFU (WAITP, or an EFU operation) stalls its whole instruction: the upper
    // operation beside it reads Q and P after the wait.
    static int Stalls(std::uint32_t lo)
    {
        if (((lo >> 25) & 0x7F) != 0x40 || (lo & 0x3F) < 0x3C)
            return 0;
        int o2 = (((lo >> 6) & 31) << 2) | (lo & 3);
        if (o2 >= 0x38 && o2 <= 0x3B)
            return 1;
        if (o2 >= 0x70 && o2 <= 0x7E && o2 != 0x77)
            return 2;
        return 0;
    }
    __attribute__((always_inline)) void Stall(std::uint32_t lo)
    {
        int k = Stalls(lo);
        if (k == 1)
            WaitQ();
        else if (k == 2)
            WaitP();
    }
    void SetQ(float v, int latency) { WaitQ(); lateQ = {cycle + (std::uint64_t)latency, v, true}; }
    void SetP(float v, int latency) { WaitP(); lateP = {cycle + (std::uint64_t)latency, v, true}; }

    // --- upper

    struct Out {
        int reg = -1;          // vf written, or -1; 32 for ACC
        float v[4];
        int dest = 0;          // field mask: bit 3 x .. bit 0 w
        bool flags = false;    // an arithmetic result: sets MAC and status
        bool clip = false;
        std::uint32_t clipValue = 0;
    };

    static int Dest(std::uint32_t w) { return (w >> 21) & 0xF; }   // x = 8, y = 4, z = 2, w = 1
    static bool Has(int dest, int field) { return dest & (8 >> field); }

    // The upper operation: decoded into what it computes (an arithmetic kind, the second
    // operand's source, the destination) and then computed field by field, so that with a
    // constant instruction word (the translated chunks) the decoding folds away.
    enum Kind { KAdd, KSub, KMadd, KMsub, KMax, KMini, KMul, KAbs, KItof, KFtoi, KOpmula, KOpmsub, KClip, KNop };
    enum Src { SVec, SBc, SQ, SI };

    __attribute__((always_inline)) Out Upper(std::uint32_t pc, std::uint32_t w)
    {
        Out o;
        int op = w & 0x3F, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31;
        int dest = Dest(w);
        Kind kind = KNop;
        Src src = SVec;
        bool toAcc = false;
        int bc = w & 3, scale = 0;
        if (op < 0x1C) {
            static constexpr Kind kBc[] = {KAdd, KSub, KMadd, KMsub, KMax, KMini, KMul};
            kind = kBc[op >> 2];
            src = SBc;
        } else if (op < 0x3C) {
            switch (op) {
            case 0x1C: kind = KMul; src = SQ; break;
            case 0x1D: kind = KMax; src = SI; break;
            case 0x1E: kind = KMul; src = SI; break;
            case 0x1F: kind = KMini; src = SI; break;
            case 0x20: kind = KAdd; src = SQ; break;
            case 0x21: kind = KMadd; src = SQ; break;
            case 0x22: kind = KAdd; src = SI; break;
            case 0x23: kind = KMadd; src = SI; break;
            case 0x24: kind = KSub; src = SQ; break;
            case 0x25: kind = KMsub; src = SQ; break;
            case 0x26: kind = KSub; src = SI; break;
            case 0x27: kind = KMsub; src = SI; break;
            case 0x28: kind = KAdd; break;
            case 0x29: kind = KMadd; break;
            case 0x2A: kind = KMul; break;
            case 0x2B: kind = KMax; break;
            case 0x2C: kind = KSub; break;
            case 0x2D: kind = KMsub; break;
            case 0x2E: kind = KOpmsub; break;
            case 0x2F: kind = KMini; break;
            default: Unknown("upper", pc, w);
            }
        } else {
            int o2 = (((w >> 6) & 31) << 2) | (w & 3);
            toAcc = true;
            if (o2 < 0x10) {
                static constexpr Kind kAcc[] = {KAdd, KSub, KMadd, KMsub};
                kind = kAcc[o2 >> 2];
                src = SBc;
            } else if (o2 >= 0x18 && o2 <= 0x1B) {
                kind = KMul;
                src = SBc;
            } else if (o2 >= 0x10 && o2 <= 0x17) {
                kind = o2 < 0x14 ? KItof : KFtoi;
                scale = o2 & 3;
                toAcc = false;
            } else {
                switch (o2) {
                case 0x1C: kind = KMul; src = SQ; break;
                case 0x1D: kind = KAbs; toAcc = false; break;
                case 0x1E: kind = KMul; src = SI; break;
                case 0x1F: kind = KClip; toAcc = false; break;
                case 0x20: kind = KAdd; src = SQ; break;
                case 0x21: kind = KMadd; src = SQ; break;
                case 0x22: kind = KAdd; src = SI; break;
                case 0x23: kind = KMadd; src = SI; break;
                case 0x24: kind = KSub; src = SQ; break;
                case 0x25: kind = KMsub; src = SQ; break;
                case 0x26: kind = KSub; src = SI; break;
                case 0x27: kind = KMsub; src = SI; break;
                case 0x28: kind = KAdd; break;
                case 0x29: kind = KMadd; break;
                case 0x2A: kind = KMul; break;
                case 0x2C: kind = KSub; break;
                case 0x2D: kind = KMsub; break;
                case 0x2E: kind = KOpmula; break;
                case 0x2F: kind = KNop; toAcc = false; break;
                default: Unknown("upper", pc, w);
                }
            }
        }
        if (kind == KNop)
            return o;
        const float* a = s.vf[fs];
        const float* b = s.vf[ft];
        if (kind == KClip) {
            float wv = std::fabs(In(b[3]));
            std::uint32_t j = 0;
            for (int k = 0; k < 3; k++) {
                float x = In(a[k]);
                if (x > wv)
                    j |= 1u << (2 * k);
                if (x < -wv)
                    j |= 2u << (2 * k);
            }
            o.clip = true;
            o.clipValue = ((LatestClip() << 6) | j) & 0xFFFFFF;
            return o;
        }
        float other = 0.0f;
        if (src == SBc)
            other = b[bc];
        else if (src == SQ)
            other = Q();
        else if (src == SI)
            other = s.i;
        o.dest = dest;
        o.reg = toAcc ? 32 : (kind == KAbs || kind == KItof || kind == KFtoi) ? ft : fd;
        o.flags = kind != KMax && kind != KMini && kind != KAbs && kind != KItof && kind != KFtoi;
        static constexpr float kScale[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
        for (int k = 0; k < 4; k++) {
            if (!Has(dest, k))
                continue;
            float x = a[k], y = src == SVec ? b[k] : other;
            float r;
            switch (kind) {
            case KAdd: r = Add(x, y); break;
            case KSub: r = Sub(x, y); break;
            case KMul: r = Mul(x, y); break;
            case KMadd: r = Add(s.acc[k], Mul(x, y)); break;
            case KMsub: r = Sub(s.acc[k], Mul(x, y)); break;
            case KMax: r = In(x) > In(y) ? In(x) : In(y); break;
            case KMini: r = In(x) < In(y) ? In(x) : In(y); break;
            case KAbs: r = std::fabs(In(x)); break;
            case KItof: r = (float)(std::int32_t)Bits(x) / kScale[scale]; break;
            case KFtoi: {
                double v = (double)In(x) * kScale[scale];
                std::int32_t n = v >= 2147483647.0 ? 0x7FFFFFFF : v <= -2147483648.0 ? (std::int32_t)0x80000000
                                                                                       : (std::int32_t)v;
                r = FromBits((std::uint32_t)n);
                break;
            }
            case KOpmula: r = k < 3 ? Mul(a[(k + 1) % 3], b[(k + 2) % 3]) : 0.0f; break;
            case KOpmsub: r = k < 3 ? Sub(s.acc[k], Mul(a[(k + 1) % 3], b[(k + 2) % 3])) : 0.0f; break;
            default: r = 0.0f; break;
            }
            o.v[k] = r;
        }
        return o;
    }

    // The MAC flags of a result's fields (zero at bit 0 + n, sign at 4 + n; n = 3 for x, 0 for w)
    // and the status flags they give (Z bit 0, S bit 1).
    __attribute__((always_inline)) void Write(const Out& o)
    {
        if (o.clip)
            PushClip(o.clipValue);
        if (o.reg < 0)
            return;
        float* r = o.reg == 32 ? s.acc : s.vf[o.reg];
        std::uint32_t mac = 0;
        for (int k = 0; k < 4; k++) {
            if (!Has(o.dest, k))
                continue;
            if (o.reg != 0)
                r[k] = o.v[k];
            if (o.flags) {
                int n = 3 - k;
                if (o.v[k] == 0.0f)
                    mac |= 1u << n;
                if (std::signbit(o.v[k]) && o.v[k] != 0.0f)
                    mac |= 1u << (4 + n);
            }
        }
        if (o.flags) {
            std::uint32_t status = ((mac & 0xF) ? 1u : 0u) | ((mac & 0xF0) ? 2u : 0u);
            PushFlags(mac, status);
        }
    }

    // --- lower

    static int S11(std::uint32_t w) { int v = w & 0x7FF; return v & 0x400 ? v - 0x800 : v; }
    float* Qword(std::uint32_t qaddr) { return reinterpret_cast<float*>(data + (qaddr & 0x3FF) * 16); }

    // Runs the lower operation; gives the branch target (in instructions) or -1.
    __attribute__((always_inline)) long Lower(std::uint32_t pc, std::uint32_t w, Out& vfWrite)
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
        auto ilw = [&](std::uint32_t qaddr) {
            float* m = Qword(qaddr);
            for (int k = 0; k < 4; k++)
                if (Has(dest, k)) {
                    viw(it, Bits(m[k]));
                    break;
                }
        };
        auto isw = [&](std::uint32_t qaddr) {
            float* m = Qword(qaddr);
            for (int k = 0; k < 4; k++)
                if (Has(dest, k))
                    m[k] = FromBits(vi(it));
        };
        std::uint32_t imm12 = ((w >> 10) & 0x800) | (w & 0x7FF), imm24 = w & 0xFFFFFF;
        switch (op) {
        case 0x00: load(it, vi(is) + S11(w)); return -1;                                    // LQ
        case 0x01: store(is, vi(it) + S11(w)); return -1;                                   // SQ
        case 0x04: ilw(vi(is) + S11(w)); return -1;                                          // ILW
        case 0x05: isw(vi(is) + S11(w)); return -1;                                          // ISW
        case 0x08: viw(it, vi(is) + (((w >> 10) & 0x7800) | (w & 0x7FF))); return -1;       // IADDIU
        case 0x09: viw(it, vi(is) - (((w >> 10) & 0x7800) | (w & 0x7FF))); return -1;       // ISUBIU
        case 0x10: s.vi[1] = Clip() == imm24 ? 1 : 0; return -1;                             // FCEQ
        case 0x11:                                                                          // FCSET
            Settle();
            s.clip = imm24;
            clipCount = 0;
            latestClip = imm24;
            latestClipSet = true;
            return -1;
        case 0x12: s.vi[1] = (Clip() & imm24) ? 1 : 0; return -1;                            // FCAND
        case 0x13: s.vi[1] = ((Clip() | imm24) & 0xFFFFFF) == 0xFFFFFF ? 1 : 0; return -1;   // FCOR
        case 0x14: viw(it, Status() == imm12 ? 1 : 0); return -1;                            // FSEQ
        case 0x15:                                                                          // FSSET
            Settle();
            s.status = (s.status & 0x3F) | (imm12 & 0xFC0);
            return -1;
        case 0x16: viw(it, Status() & imm12); return -1;                                     // FSAND
        case 0x17: viw(it, Status() | imm12); return -1;                                     // FSOR
        case 0x18: viw(it, Mac() == vi(is) ? 1 : 0); return -1;                              // FMEQ
        case 0x1A: viw(it, Mac() & vi(is)); return -1;                                       // FMAND
        case 0x1B: viw(it, (Mac() | vi(is)) & 0xFFFF); return -1;                            // FMOR
        case 0x1C: viw(it, Clip() & 0xFFF); return -1;                                       // FCGET
        case 0x20: return target();                                                          // B
        case 0x21: viw(it, pc + 2); return target();                                         // BAL
        case 0x24: return vi(is);                                                            // JR
        case 0x25: { long t = vi(is); viw(it, pc + 2); return t; }                           // JALR
        case 0x28: return vi(it) == vi(is) ? target() : -1;                                  // IBEQ
        case 0x29: return vi(it) != vi(is) ? target() : -1;                                  // IBNE
        case 0x2C: return (std::int16_t)vi(is) < 0 ? target() : -1;                          // IBLTZ
        case 0x2D: return (std::int16_t)vi(is) > 0 ? target() : -1;                          // IBGTZ
        case 0x2E: return (std::int16_t)vi(is) <= 0 ? target() : -1;                         // IBLEZ
        case 0x2F: return (std::int16_t)vi(is) >= 0 ? target() : -1;                         // IBGEZ
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
        const float* fsv = s.vf[is];
        float fsf = In(fsv[(w >> 21) & 3]), ftf = In(s.vf[it][(w >> 23) & 3]);
        switch (o2) {
        case 0x30:   // MOVE (and the lower NOP, MOVE to vf0)
            vfWrite.reg = it;
            vfWrite.dest = dest;
            std::memcpy(vfWrite.v, s.vf[is], 16);
            return -1;
        case 0x31:   // MR32: x gets y, y z, z w, w x
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
            Settle();
            float r;
            if (ftf == 0.0f) {
                s.status |= fsf == 0.0f ? 0x410 : 0x820;   // I (0/0) or D, and their sticky bits
                r = (std::signbit(fsf) != std::signbit(ftf)) ? -3.4028235e38f : 3.4028235e38f;
            } else {
                r = Fix(fsf / ftf);
            }
            SetQ(r, 7);
            return -1;
        }
        case 0x39: SetQ(Fix(std::sqrt(std::fabs(ftf))), 7); return -1;                        // SQRT
        case 0x3A: {                                                                          // RSQRT
            float d = std::sqrt(std::fabs(ftf));
            SetQ(d == 0.0f ? (std::signbit(fsf) ? -3.4028235e38f : 3.4028235e38f) : Fix(fsf / d), 13);
            return -1;
        }
        case 0x3B: WaitQ(); return -1;                                                        // WAITQ
        case 0x3C: viw(it, Bits(fsv[(w >> 21) & 3])); return -1;                              // MTIR
        case 0x3D:                                                                            // MFIR
            vfWrite.reg = it;
            vfWrite.dest = dest;
            for (int k = 0; k < 4; k++)
                vfWrite.v[k] = FromBits((std::uint32_t)(std::int32_t)(std::int16_t)vi(is));
            return -1;
        case 0x3E: { std::uint32_t q = vi(is); ilw(q); return -1; }                           // ILWR
        case 0x3F: { std::uint32_t q = vi(is); isw(q); return -1; }                           // ISWR
        case 0x64:                                                                            // MFP
            vfWrite.reg = it;
            vfWrite.dest = dest;
            for (int k = 0; k < 4; k++)
                vfWrite.v[k] = P();
            return -1;
        case 0x68: viw(it, ctx.top); return -1;                                              // XTOP
        case 0x69: viw(it, ctx.itop); return -1;                                             // XITOP
        case 0x6C:                                                                            // XGKICK
            if (ctx.kick)
                ctx.kick(vi(is) & 0x3FF, ctx.user);
            return -1;
        case 0x7B: WaitP(); return -1;                                                        // WAITP
        }
        // The EFU's operations, writing P after their latencies.
        float x = In(fsv[0]), y = In(fsv[1]), z = In(fsv[2]), wv = In(fsv[3]);
        float sum2 = Add(Add(Mul(x, x), Mul(y, y)), Mul(z, z));
        switch (o2) {
        case 0x70: SetP(sum2, 11); return -1;                                                     // ESADD
        case 0x71: SetP(sum2 == 0.0f ? 3.4028235e38f : Fix(1.0f / sum2), 18); return -1;          // ERSADD
        case 0x72: SetP(Fix(std::sqrt(sum2)), 18); return -1;                                      // ELENG
        case 0x73: SetP(sum2 == 0.0f ? 3.4028235e38f : Fix(1.0f / std::sqrt(sum2)), 24); return -1; // ERLENG
        case 0x74: SetP(Fix(std::atan2(y, x)), 54); return -1;                                     // EATANxy
        case 0x75: SetP(Fix(std::atan2(z, x)), 54); return -1;                                     // EATANxz
        case 0x76: SetP(Add(Add(Add(x, y), z), wv), 12); return -1;                                // ESUM
        case 0x78: SetP(Fix(std::sqrt(std::fabs(fsf))), 12); return -1;                            // ESQRT
        case 0x79: {                                                                              // ERSQRT
            float d = std::sqrt(std::fabs(fsf));
            SetP(d == 0.0f ? 3.4028235e38f : Fix(1.0f / d), 18);
            return -1;
        }
        case 0x7A: SetP(fsf == 0.0f ? 3.4028235e38f : Fix(1.0f / fsf), 12); return -1;              // ERCPR
        case 0x7C: SetP(Fix(std::sin(fsf)), 29); return -1;                                        // ESIN
        case 0x7D: SetP(Fix(std::atan(fsf)), 54); return -1;                                       // EATAN
        case 0x7E: SetP(Fix(std::exp(-fsf)), 44); return -1;                                       // EEXP
        }
        Unknown("lower", pc, w);
    }
};


// One instruction at pc, as the interpreter runs it: pc moves on (after a delay slot, to a taken
// branch's target); done is set after the end bit's one more instruction.
inline void Step(Runner& r, std::uint32_t& pc)
{
    const std::uint32_t* micro = Micro();
    std::uint32_t lo = micro[pc * 2], hi = micro[pc * 2 + 1];
    if (!(hi & 0x80000000u))
        r.Stall(lo);
    Runner::Out up = r.Upper(pc, hi);
    Runner::Out low;
    long next = -1;
    if (hi & 0x80000000u)
        std::memcpy(&r.s.i, &lo, 4);   // the I bit: the lower word is the float for I
    else
        next = r.Lower(pc, lo, low);
    r.Write(low);
    r.Write(up);
    r.cycle++;
    r.executed++;
    bool end = r.ending;
    r.ending = (hi & 0x40000000u) != 0;
    long to = r.branch >= 0 ? r.branch : (long)pc + 1;
    r.branch = next;
    pc = (std::uint32_t)to & 0x7FF;
    if (end)
        r.done = true;
}

// A chunk of microcode translated to C++ (tools/vu1_translate.py): entered at an instruction of
// its range with no branch or end pending, it runs until it leaves its range, ends the program
// or meets what it leaves to the interpreter, and gives the next pc (with r.branch, r.ending and
// r.done as Step leaves them).
typedef std::uint32_t (*ChunkFn)(Runner& r, std::uint32_t pc);
struct Chunk {
    std::uint32_t addr, count, hash;
    ChunkFn fn;
};
extern const Chunk kTranslated[];
extern const std::size_t kTranslatedCount;

}  // namespace core
}  // namespace vu1
}  // namespace port

#endif
