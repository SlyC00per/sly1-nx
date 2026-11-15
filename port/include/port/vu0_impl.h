// SPDX-License-Identifier: AGPL-3.0-or-later
// Portable bodies of the decompilation's VU0 helpers (sly1-decomp include/vu0.h). tools/gen.py
// keeps that header as it is, with every helper's name, signature and overload, and replaces each
// body that was VU0 assembly with a call of the function of the same name here. Each function
// computes what the helper's "Computes:" line in vu0.h states, lane by lane:
//
// - Lanes x, y, z, w are the u128's four 32-bit words (little-endian), floats.
// - A lane the line calls "not defined" is 0 here (no caller reads it), except where the line
//   says what the hardware leaves there.
// - Sums are formed in the order the hardware's sequence forms them (the accumulator chains), and
//   a product is rounded before it is added (no fused multiply-add: the game code is compiled
//   with -ffp-contract=off).
// - VU0 arithmetic has no infinity or NaN and no denormals: a result that would be infinite or
//   NaN is the largest finite float with its sign, a denormal is 0 (Fix). VU0 rounds toward zero:
//   the platform layer sets that rounding mode on the game's thread, so the host's arithmetic here
//   rounds the same way. These rules come from public descriptions of the hardware and are not
//   proved by the decompilation (docs/PORTING.md there): they are tested, not assumed exact.
#ifndef PORT_VU0_IMPL_H
#define PORT_VU0_IMPL_H

#include <cmath>
#include <cstdint>
#include <cstring>

namespace port_vu0 {

typedef unsigned int u128 __attribute__((mode(TI)));
typedef float f32;
typedef std::uint32_t u32;

struct L {
    f32 v[4];
};

inline L Ld(u128 a)
{
    L l;
    std::memcpy(l.v, &a, 16);
    return l;
}

inline u128 St(const L& l)
{
    u128 r;
    std::memcpy(&r, l.v, 16);
    return r;
}

inline u32 Bits(f32 f)
{
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

inline f32 FromBits(u32 b)
{
    f32 f;
    std::memcpy(&f, &b, 4);
    return f;
}

// VU0's float rules on one result: no infinity or NaN (the largest finite float, with the sign),
// no denormals (0, with the sign).
inline f32 Fix(f32 x)
{
    u32 b = Bits(x);
    u32 e = (b >> 23) & 0xFF;
    if (e == 0xFF)
        return FromBits((b & 0x80000000u) | 0x7F7FFFFFu);
    if (e == 0)
        return FromBits(b & 0x80000000u);
    return x;
}

inline f32 Mul(f32 a, f32 b) { return Fix(a * b); }
inline f32 Add(f32 a, f32 b) { return Fix(a + b); }
inline f32 Sub(f32 a, f32 b) { return Fix(a - b); }

inline L Lanes(f32 x, f32 y, f32 z, f32 w) { return L{{x, y, z, w}}; }

// --- constants and moves

inline u128 Zero() { return St(Lanes(0, 0, 0, 0)); }
inline u128 Ones() { return St(Lanes(1, 1, 1, 1)); }
inline u128 Copy(u128 a) { return a; }
inline u128 FromFloat(f32 f) { return St(Lanes(f, 0, 0, 0)); }
inline u128 FromFloatAt(const f32& f) { return FromFloat(f); }
inline u128 ScalarAtValue(const f32 f) { return FromFloat(f); }
inline u128 FromFloatAtCall(f32 f) { return FromFloat(f); }
inline u128 ScalarInGpr(f32 f) { return FromFloat(f); }
inline u128 ScalarInGprAt(const f32& f) { return FromFloat(f); }
inline u128 ScalarMovedReg(f32 f) { return FromFloat(f); }
inline f32 ToFloat(u128 a) { return Ld(a).v[0]; }
inline f32 ToFloatMoved(u128 a) { return Ld(a).v[0]; }
inline f32 ToFloatNow(u128 a) { return Ld(a).v[0]; }
inline f32 ToFloatViaBits(u128 a) { return Ld(a).v[0]; }
inline f32 ToFloatY(u128 a) { return Ld(a).v[1]; }
inline f32 ToFloatZ(u128 a) { return Ld(a).v[2]; }
inline u32 ToBits(u128 a) { return Bits(Ld(a).v[0]); }
template <class T> inline void StoreX(T& dst, u128 a)
{
    static_assert(sizeof(T) == 4, "a word store");
    u32 b = ToBits(a);
    std::memcpy(&dst, &b, 4);
}
template <class T> inline void StoreX(T& dst, u32 bits)
{
    static_assert(sizeof(T) == 4, "a word store");
    std::memcpy(&dst, &bits, 4);
}
inline u128 Splat(f32 f) { return St(Lanes(f, f, f, f)); }
// w is z's sign extension, as the MMI shuffle leaves it.
inline u128 FromFloats(f32 x, f32 y, f32 z)
{
    return St(Lanes(x, y, z, FromBits((Bits(z) & 0x80000000u) ? 0xFFFFFFFFu : 0)));
}

// --- arithmetic

inline u128 Add(u128 a, u128 b)
{
    L p = Ld(a), q = Ld(b);
    return St(Lanes(Add(p.v[0], q.v[0]), Add(p.v[1], q.v[1]), Add(p.v[2], q.v[2]), Add(p.v[3], q.v[3])));
}

inline u128 Sub(u128 a, u128 b)
{
    L p = Ld(a), q = Ld(b);
    return St(Lanes(Sub(p.v[0], q.v[0]), Sub(p.v[1], q.v[1]), Sub(p.v[2], q.v[2]), Sub(p.v[3], q.v[3])));
}

inline u128 MulX(u128 a, u128 s)
{
    L p = Ld(a);
    f32 k = Ld(s).v[0];
    return St(Lanes(Mul(p.v[0], k), Mul(p.v[1], k), Mul(p.v[2], k), Mul(p.v[3], k)));
}

// a + b*s.x, with VU0's vf0 (0, 0, 0, 1) added to a first: w gains 1.
inline u128 MulAddX(u128 a, u128 b, u128 s)
{
    L p = Ld(a), q = Ld(b);
    f32 k = Ld(s).v[0];
    L r;
    for (int i = 0; i < 4; i++)
        r.v[i] = Add(Add(p.v[i], i == 3 ? 1.0f : 0.0f), Mul(q.v[i], k));
    return St(r);
}

inline u128 MulAddX2(u128 a, u128 b, u128 s, u128 c, u128 t)
{
    L p = Ld(a), q = Ld(b), u = Ld(c);
    f32 k = Ld(s).v[0], m = Ld(t).v[0];
    L r;
    for (int i = 0; i < 4; i++)
        r.v[i] = Add(Add(Add(p.v[i], i == 3 ? 1.0f : 0.0f), Mul(q.v[i], k)), Mul(u.v[i], m));
    return St(r);
}

inline u128 MulAddX2WeightFirst(u128 a, u128 s, u128 b, u128 t, u128 c) { return MulAddX2(a, b, s, c, t); }

inline u128 MulAddX3(u128 a, u128 s, u128 b, u128 t, u128 c, u128 u, u128 d)
{
    L p = Ld(a), q = Ld(b), w = Ld(c), e = Ld(d);
    f32 k = Ld(s).v[0], m = Ld(t).v[0], n = Ld(u).v[0];
    L r;
    for (int i = 0; i < 4; i++)
        r.v[i] = Add(Add(Add(Add(p.v[i], i == 3 ? 1.0f : 0.0f), Mul(q.v[i], k)), Mul(w.v[i], m)), Mul(e.v[i], n));
    return St(r);
}

inline u128 Dot(u128 a, u128 b)
{
    L p = Ld(a), q = Ld(b);
    f32 x = Add(Add(Mul(p.v[0], q.v[0]), Mul(p.v[1], q.v[1])), Mul(p.v[2], q.v[2]));
    return St(Lanes(x, Mul(p.v[1], q.v[1]), Mul(p.v[2], q.v[2]), 0));
}

inline u128 DotXY(u128 a, u128 b)
{
    L p = Ld(a), q = Ld(b);
    f32 y = Mul(p.v[1], q.v[1]);
    return St(Lanes(Add(Mul(p.v[0], q.v[0]), y), y, 0, 0));
}

inline u128 SelfDot(u128 v) { return Dot(v, v); }

// vsqrt: the root of the absolute value, never NaN.
inline u128 Sqrt(u128 a) { return St(Lanes(Fix(std::sqrt(std::fabs(Ld(a).v[0]))), 0, 0, 0)); }

// Q = 1 / s.x, saturating to the largest float with s.x's sign when s.x is zero (also 0/0); then
// every lane of a times Q.
inline f32 Reciprocal(f32 d)
{
    if (d == 0.0f)
        return FromBits((Bits(d) & 0x80000000u) | 0x7F7FFFFFu);
    return Fix(1.0f / d);
}

inline u128 DivX(u128 a, u128 s)
{
    f32 q = Reciprocal(Ld(s).v[0]);
    L p = Ld(a);
    return St(Lanes(Mul(p.v[0], q), Mul(p.v[1], q), Mul(p.v[2], q), Mul(p.v[3], q)));
}

inline u128 Cross(u128 a, u128 b)
{
    L p = Ld(a), q = Ld(b);
    return St(Lanes(Sub(Mul(p.v[1], q.v[2]), Mul(p.v[2], q.v[1])),
                    Sub(Mul(p.v[2], q.v[0]), Mul(p.v[0], q.v[2])),
                    Sub(Mul(p.v[0], q.v[1]), Mul(p.v[1], q.v[0])), 0));
}

inline u128 Min(u128 a, u128 b)
{
    L p = Ld(a), q = Ld(b);
    return St(Lanes(p.v[0] < q.v[0] ? p.v[0] : q.v[0], p.v[1] < q.v[1] ? p.v[1] : q.v[1],
                    p.v[2] < q.v[2] ? p.v[2] : q.v[2], 0));
}

inline u128 Max(u128 a, u128 b)
{
    L p = Ld(a), q = Ld(b);
    return St(Lanes(p.v[0] > q.v[0] ? p.v[0] : q.v[0], p.v[1] > q.v[1] ? p.v[1] : q.v[1],
                    p.v[2] > q.v[2] ? p.v[2] : q.v[2], 0));
}

// --- matrices: rows are quadwords; ACC = r0*v.x, ACC += r1*v.y, ... in that order

inline u128 RotateRows(u128 r0, u128 r1, u128 r2, u128 v)
{
    L a = Ld(r0), b = Ld(r1), c = Ld(r2), x = Ld(v);
    L r;
    for (int i = 0; i < 3; i++)
        r.v[i] = Add(Add(Mul(a.v[i], x.v[0]), Mul(b.v[i], x.v[1])), Mul(c.v[i], x.v[2]));
    r.v[3] = 0;
    return St(r);
}

inline u128 RotateRowsTied(u128 r0, u128 r1, u128 r2, u128 v)
{
    L r = Ld(RotateRows(r0, r1, r2, v));
    r.v[3] = Ld(v).v[3];
    return St(r);
}

template <class M> inline const u128* Rows(const M& m) { return reinterpret_cast<const u128*>(&m); }

template <class M> inline u128 Rotate(const M& m, u128 v)
{
    const u128* r = Rows(m);
    return RotateRowsTied(r[0], r[1], r[2], v);
}

template <class M> inline u128 RotateMembers(const M& m, u128 v) { return Rotate(m, v); }

inline u128 TransformRows(u128 r0, u128 r1, u128 r2, u128 r3, u128 v, f32 w)
{
    L a = Ld(r0), b = Ld(r1), c = Ld(r2), d = Ld(r3), x = Ld(v);
    L r;
    for (int i = 0; i < 3; i++)
        r.v[i] = Add(Add(Add(Mul(a.v[i], x.v[0]), Mul(b.v[i], x.v[1])), Mul(c.v[i], x.v[2])), Mul(d.v[i], w));
    r.v[3] = 0;
    return St(r);
}

template <class M> inline u128 Transform(const M& m, u128 v, f32 w)
{
    const u128* r = Rows(m);
    return TransformRows(r[0], r[1], r[2], r[3], v, w);
}

inline u128 Transform4(u128 m0, u128 m1, u128 m2, u128 m3, u128 v)
{
    L a = Ld(m0), b = Ld(m1), c = Ld(m2), d = Ld(m3), x = Ld(v);
    L r;
    for (int i = 0; i < 4; i++)
        r.v[i] = Add(Add(Add(Mul(a.v[i], x.v[0]), Mul(b.v[i], x.v[1])), Mul(c.v[i], x.v[2])), Mul(d.v[i], x.v[3]));
    return St(r);
}

// out[c] = b[0]*a[0].c + b[1]*a[1].c + b[2]*a[2].c, all four lanes, for c = x, y, z.
template <class M> inline void MulTransposed(M& out, const M& a, const M& b)
{
    const u128* ra = Rows(a);
    const u128* rb = Rows(b);
    L a0 = Ld(ra[0]), a1 = Ld(ra[1]), a2 = Ld(ra[2]);
    L b0 = Ld(rb[0]), b1 = Ld(rb[1]), b2 = Ld(rb[2]);
    u128 res[3];
    for (int c = 0; c < 3; c++) {
        L r;
        for (int i = 0; i < 4; i++)
            r.v[i] = Add(Add(Mul(b0.v[i], a0.v[c]), Mul(b1.v[i], a1.v[c])), Mul(b2.v[i], a2.v[c]));
        res[c] = St(r);
    }
    std::memcpy(reinterpret_cast<u128*>(&out), res, sizeof res);   // the inputs may be out itself
}

template <class M> inline u128 RotateTransposed(const M& m, u128 v)
{
    const u128* r = Rows(m);
    L x = Ld(v);
    L out;
    for (int k = 0; k < 3; k++) {
        L row = Ld(r[k]);
        out.v[k] = Add(Add(Mul(row.v[0], x.v[0]), Mul(row.v[1], x.v[1])), Mul(row.v[2], x.v[2]));
    }
    out.v[3] = 0;
    return St(out);
}

template <class M> inline u128 RotateTransposedMembers(const M& m, u128 v) { return RotateTransposed(m, v); }

inline u128 MulXSum(u128 a, u128 s, u128 b, u128 t)
{
    L p = Ld(a), q = Ld(b);
    f32 k = Ld(s).v[0], m = Ld(t).v[0];
    L r;
    for (int i = 0; i < 4; i++)
        r.v[i] = Add(Mul(p.v[i], k), Mul(q.v[i], m));
    return St(r);
}

inline u128 MulXSum3(u128 a, u128 s, u128 b, u128 t, u128 c, u128 u)
{
    L p = Ld(a), q = Ld(b), w = Ld(c);
    f32 k = Ld(s).v[0], m = Ld(t).v[0], n = Ld(u).v[0];
    L r;
    for (int i = 0; i < 4; i++)
        r.v[i] = Add(Add(Mul(p.v[i], k), Mul(q.v[i], m)), Mul(w.v[i], n));
    return St(r);
}

inline u128 MulXSum4(u128 a, u128 s, u128 b, u128 t, u128 c, u128 u, u128 d, u128 v)
{
    L p = Ld(a), q = Ld(b), w = Ld(c), e = Ld(d);
    f32 k = Ld(s).v[0], m = Ld(t).v[0], n = Ld(u).v[0], o = Ld(v).v[0];
    L r;
    for (int i = 0; i < 4; i++)
        r.v[i] = Add(Add(Add(Mul(p.v[i], k), Mul(q.v[i], m)), Mul(w.v[i], n)), Mul(e.v[i], o));
    return St(r);
}

// The MAC flags' sign bits of d_k = r0.k*v.x + r1.k*v.y + r2.k*v.z - ones.k*s.x: bit 7 for x,
// 6 for y, 5 for z, 4 for w (callers compare the whole value with 0xF0).
inline u32 PlaneSigns(u128 r0, u128 r1, u128 r2, u128 v, u128 ones, u128 s)
{
    L a = Ld(r0), b = Ld(r1), c = Ld(r2), x = Ld(v), o = Ld(ones);
    f32 k = Ld(s).v[0];
    u32 flags = 0;
    for (int i = 0; i < 4; i++) {
        f32 d = Sub(Add(Add(Mul(a.v[i], x.v[0]), Mul(b.v[i], x.v[1])), Mul(c.v[i], x.v[2])), Mul(o.v[i], k));
        if (Bits(d) & 0x80000000u && d != 0.0f)
            flags |= 0x80u >> i;
    }
    return flags;
}

// Rows of the transpose of a 3x3 matrix held as three quadword rows (their w ignored), w zero:
// out.r[k] = (m0.k, m1.k, m2.k, 0). R is vu0.h's Rows.
template <class R> inline R Transpose(const R& m)
{
    const u128* r = Rows(m);
    L a = Ld(r[0]), b = Ld(r[1]), c = Ld(r[2]);
    R out;
    u128* o = reinterpret_cast<u128*>(&out);
    for (int k = 0; k < 3; k++)
        o[k] = St(Lanes(a.v[k], b.v[k], c.v[k], 0));
    return out;
}

// --- the VU0 microprograms (not decompiled; their two callers get the port's own code)

[[noreturn]] void Microprogram(const char* what);

template <int N> inline void LoadVf(const void*) { Microprogram("LoadVf"); }
template <int N> inline void CallMs() { Microprogram("CallMs"); }
template <int N> inline void MoveToVi(u32) { Microprogram("MoveToVi"); }
template <int N> inline u32 ReadViWait() { Microprogram("ReadViWait"); }
inline u32 NextLightGroup(u32, u32) { Microprogram("NextLightGroup"); }
inline void ParticleStart(u128, u128, u128, u128, u128, u128, u128, u128, u128) { Microprogram("ParticleFilter::Start"); }
inline void ParticleNext(u128, u128) { Microprogram("ParticleFilter::Next"); }
inline void ParticleWait() { Microprogram("ParticleFilter::Wait"); }
inline void ParticleResults(u128&, u128&) { Microprogram("ParticleFilter::Results"); }
inline void ParticleBounds(u128&, u128&) { Microprogram("ParticleFilter::Bounds"); }

}  // namespace port_vu0

#endif
