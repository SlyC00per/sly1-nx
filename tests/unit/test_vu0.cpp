// SPDX-License-Identifier: AGPL-3.0-or-later
// Unit tests of the VU0 helpers' portable bodies (port/vu0_impl.h) against the "Computes:" lines
// of the decompilation's include/vu0.h, above all the lines that state the hardware's own rules.
#include "port/vu0_impl.h"
#include "unit.h"

#include <cfloat>

using namespace port_vu0;

namespace {

u128 V(f32 x, f32 y, f32 z, f32 w) { return St(Lanes(x, y, z, w)); }
f32 X(u128 a) { return Ld(a).v[0]; }
f32 Y(u128 a) { return Ld(a).v[1]; }
f32 Z(u128 a) { return Ld(a).v[2]; }
f32 W(u128 a) { return Ld(a).v[3]; }

struct alignas(16) Mat { u128 r[4]; };

}  // namespace

UNIT_TEST(vu0_lanes_and_moves)
{
    u128 a = V(1, 2, 3, 4);
    CHECK(X(a) == 1 && Y(a) == 2 && Z(a) == 3 && W(a) == 4);
    CHECK(ToFloat(FromFloat(2.5f)) == 2.5f);
    CHECK(ToBits(FromFloat(1.0f)) == 0x3F800000u);
    u128 s = Splat(7);
    CHECK(X(s) == 7 && W(s) == 7);
    f32 out = 0;
    StoreX(out, a);
    CHECK(out == 1);
    CHECK(Bits(W(FromFloats(1, 2, -3))) == 0xFFFFFFFFu);  // z's sign extension
}

UNIT_TEST(vu0_arithmetic)
{
    u128 a = V(1, 2, 3, 4), b = V(10, 20, 30, 40);
    CHECK(X(Add(a, b)) == 11 && W(Add(a, b)) == 44);
    CHECK(Z(Sub(b, a)) == 27);
    CHECK(Y(MulX(a, FromFloat(3))) == 6 && W(MulX(a, FromFloat(3))) == 12);
    // MulAddX adds vf0's 1 to w.
    u128 m = MulAddX(a, b, FromFloat(2));
    CHECK(X(m) == 21 && Z(m) == 63 && W(m) == 4 + 1 + 80);
    CHECK(X(Dot(a, b)) == 10 + 40 + 90);
    CHECK(X(DotXY(a, b)) == 50);
    u128 c = Cross(V(1, 0, 0, 9), V(0, 1, 0, 9));
    CHECK(X(c) == 0 && Y(c) == 0 && Z(c) == 1 && W(c) == 0);
    CHECK(X(MulXSum(a, FromFloat(0.5f), b, FromFloat(0.5f))) == 5.5f);
}

UNIT_TEST(vu0_hardware_rules)
{
    // vsqrt: the root of the absolute value, never NaN.
    CHECK(X(Sqrt(FromFloat(-16))) == 4);
    // vdiv by zero: the largest float with the zero's sign, then the multiply.
    CHECK(X(DivX(V(2, 0, 0, 0), FromFloat(0.0f))) == FLT_MAX);
    CHECK(X(DivX(V(1, 0, 0, 0), FromFloat(-0.0f))) == -FLT_MAX);
    CHECK(X(DivX(V(6, 0, 0, 0), FromFloat(2))) == 3);
    // No infinity, no denormals.
    CHECK(X(MulX(V(FLT_MAX, 0, 0, 0), FromFloat(2))) == FLT_MAX);
    CHECK(X(MulX(V(-FLT_MAX, 0, 0, 0), FromFloat(2))) == -FLT_MAX);
    CHECK(X(MulX(V(1e-30f, 0, 0, 0), FromFloat(1e-15f))) == 0.0f);
}

UNIT_TEST(vu0_matrices)
{
    Mat m;
    m.r[0] = V(1, 0, 0, 0);
    m.r[1] = V(0, 2, 0, 0);
    m.r[2] = V(0, 0, 3, 0);
    m.r[3] = V(10, 20, 30, 1);
    u128 v = V(1, 1, 1, 7);
    u128 r = Rotate(m, v);
    CHECK(X(r) == 1 && Y(r) == 2 && Z(r) == 3 && W(r) == 7);   // w from v
    u128 t = Transform(m, v, 1.0f);
    CHECK(X(t) == 11 && Y(t) == 22 && Z(t) == 33);
    u128 p = Transform4(m.r[0], m.r[1], m.r[2], m.r[3], V(1, 1, 1, 2));
    CHECK(X(p) == 21 && W(p) == 2);
    // RotateTransposed: each row dotted with v.
    Mat q;
    q.r[0] = V(1, 2, 3, 0);
    q.r[1] = V(4, 5, 6, 0);
    q.r[2] = V(7, 8, 9, 0);
    u128 d = RotateTransposed(q, V(1, 0, 1, 0));
    CHECK(X(d) == 4 && Y(d) == 10 && Z(d) == 16);
    struct alignas(16) Rows3 { u128 r[3]; } rows = {{q.r[0], q.r[1], q.r[2]}};
    Rows3 tr = Transpose(rows);
    CHECK(X(tr.r[0]) == 1 && Y(tr.r[0]) == 4 && Z(tr.r[0]) == 7 && W(tr.r[0]) == 0);
    CHECK(Z(tr.r[2]) == 9);
}

UNIT_TEST(vu0_plane_signs)
{
    // d_k = r0.k*v.x + r1.k*v.y + r2.k*v.z - s.x: negative in x and z only.
    u128 r0 = V(-1, 1, -1, 1), r1 = V(0, 0, 0, 0), r2 = V(0, 0, 0, 0);
    u32 f = PlaneSigns(r0, r1, r2, V(1, 0, 0, 0), Ones(), FromFloat(0.5f));
    CHECK(f == (0x80u | 0x20u));
}

// --- the micro-mode interpreter (port/vu0_micro.h): the four-instruction MAC flag latency the
// game's light tests are scheduled around, and the end bit's one more instruction.
#include "port/vu0_micro.h"

UNIT_TEST(vu0_micro_flag_latency_and_end)
{
    using port::vu0::Vu0;
    port::vu0::State& s = Vu0();
    const unsigned kNopUpper = (0x0Bu << 6) | 0x3Fu, kNopLower = 0x8000033Cu, kEnd = 0x40000000u;
    const unsigned subX = (1u << 24) | (3u << 16) | (2u << 11) | (1u << 6) | 0x2Cu;   // sub.x vf1, vf2, vf3
    auto fmeq = [](unsigned it, unsigned is) { return (0x18u << 25) | (it << 16) | (is << 11); };
    const unsigned prog[][2] = {
        {kNopLower, subX},              // 0: the x result is negative: MAC 0x80 from instruction 4
        {kNopLower, kNopUpper},
        {kNopLower, kNopUpper},
        {fmeq(7, 6), kNopUpper},        // 3: still the old flags
        {fmeq(5, 6), kNopUpper | kEnd}, // 4: the new ones; the end bit
        {(0x08u << 25) | (8u << 16) | 9u, kNopUpper},   // 5: iaddiu vi8, vi0, 9 (runs: one more)
        {(0x08u << 25) | (8u << 16) | 1u, kNopUpper},   // 6: not reached
    };
    for (unsigned k = 0; k < 7; k++) {
        s.micro[k * 2] = prog[k][0];
        s.micro[k * 2 + 1] = prog[k][1];
    }
    s.vf[2][0] = 1.0f;
    s.vf[3][0] = 2.0f;
    s.vi[6] = 0x80;
    s.mac = 0;
    s.vi[5] = s.vi[7] = s.vi[8] = 0xFFFF;
    port::vu0::Run(0);
    CHECK(s.vf[1][0] == -1.0f);
    CHECK(s.vi[7] == 0);
    CHECK(s.vi[5] == 1);
    CHECK(s.vi[8] == 9);
}
