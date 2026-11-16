// SPDX-License-Identifier: AGPL-3.0-or-later
// The native handlers (port/render/vu1_handlers.h). Each is written from the entry's microcode
// and the builder that packs its input, as docs/RENDERER.md describes, and checked against the
// interpreter with SLY1_VU1_CHECK before it is relied on.
//
// A handler computes what the microcode computes with the same operations in the same order (the
// VU's arithmetic, port/render/vu1_runner.h), so its results are the same bits; it leaves the
// state a later program reads (port/render/vu1_live.h) as the microcode leaves it, and skips the
// rest: the software pipelining, the reads ahead past the buffer's end, the scratch registers.
#include "vu1_handlers.h"

#include <cmath>
#include <cstring>

namespace port {
namespace vu1 {

namespace {

using core::Add;
using core::Bits;
using core::FromBits;
using core::In;
using core::Mul;
using core::Runner;
using core::Sub;

// The VU's arithmetic four fields at a time (the same rules as core::Add, Sub, Mul field by
// field: operands and results with the largest exponent made the largest finite float, denormals
// 0; the host rounds toward zero on the game's thread, vector registers included).
typedef float F4 __attribute__((vector_size(16)));
typedef std::uint32_t U4 __attribute__((vector_size(16)));
typedef std::int32_t I4 __attribute__((vector_size(16)));

inline F4 Fix4(F4 x)
{
    U4 b = (U4)x;
    U4 e = b & 0x7F800000u, sign = b & 0x80000000u;
    U4 big = (U4)(e == 0x7F800000u), tiny = (U4)(e == 0u);
    b = (b & ~big) | ((sign | 0x7F7FFFFFu) & big);
    b = (b & ~tiny) | (sign & tiny);
    return (F4)b;
}
inline F4 Mul4(F4 a, F4 b) { return Fix4(Fix4(a) * Fix4(b)); }
inline F4 Add4(F4 a, F4 b) { return Fix4(Fix4(a) + Fix4(b)); }
inline F4 Splat(float f) { return F4{f, f, f, f}; }

union V {
    F4 v;
    struct { float x, y, z, w; };
    float a[4];
    float& operator[](int k) { return a[k]; }
    float operator[](int k) const { return a[k]; }
};

V Load(Runner& r, std::uint32_t qaddr)
{
    V v;
    std::memcpy(&v, r.Qword(qaddr), 16);
    return v;
}

void Store(Runner& r, std::uint32_t qaddr, const V& v) { std::memcpy(r.Qword(qaddr), &v, 16); }

// vi registers are 16 bits; ILW and MTIR take a word's low half
std::uint16_t Half(float f) { return (std::uint16_t)Bits(f); }

// A position through the matrix rows m[0..3] (MULAx, MADDAy, MADDAz, MADDw with vf00w).
V Transform(const V* m, const V& p)
{
    V o;
    F4 acc = Mul4(m[0].v, Splat(p.x));
    acc = Add4(acc, Mul4(m[1].v, Splat(p.y)));
    acc = Add4(acc, Mul4(m[2].v, Splat(p.z)));
    o.v = Add4(acc, Mul4(m[3].v, Splat(1.0f)));
    return o;
}

// CLIP's six bits for p against its w.
std::uint32_t ClipCode(const V& p)
{
    float wv = std::fabs(In(p.w));
    std::uint32_t j = 0;
    for (int k = 0; k < 3; k++) {
        float x = In(p[k]);
        if (x > wv)
            j |= 1u << (2 * k);
        if (x < -wv)
            j |= 2u << (2 * k);
    }
    return j;
}

// DIV Q, vf00w, d: 1 / d, or the largest float with d's sign and the D flags for 0.
float Reciprocal(Runner& r, float d)
{
    float ftf = In(d);
    if (ftf == 0.0f) {
        r.s.status |= 0x820;
        return std::signbit(ftf) ? -3.4028235e38f : 3.4028235e38f;
    }
    return core::Fix(1.0f / ftf);
}

float Mini(float a, float b) { return In(a) < In(b) ? In(a) : In(b); }

// FCOR's six planes over the last three vertices' clip codes (bits 0-17): whether all three are
// outside one plane, as 0x274's tests find it.
bool AllOutsideOnePlane(std::uint32_t codes)
{
    for (int bit = 0; bit < 6; bit++) {
        std::uint32_t m = (1u << bit) | (1u << (bit + 6)) | (1u << (bit + 12));
        if ((codes & m) == m)
            return true;
    }
    return false;
}

// --- 0x356 of chunk 79A6F5FB: a lit batch's vertices transformed in place.
//
// Input, from TOP: 8 the screen parameters (x a control word 32768 is added to, y the scale of
// 1/w, z the largest w, w another control word), 9.z whether backfaces are marked, 11.x and 11.y
// the first vertex and the end (offsets from TOP), 14-17 the matrix; at 0 and 1 of data memory
// the viewport's scale and offset. A vertex is four quadwords: 0 its STQ, 3 its position with
// its control word in w.
//
// Each vertex: transformed (clip coordinates), clipped against w (the codes of the last three
// kept), divided by w (clamped to 8.z), STQ times 1/w; the screen position is the clip position
// times 1/w times the scale plus the offset, its w the scaled 1/w plus the control word. From the
// third vertex on, the triangle the vertex closes is tested (0x274): when any of its vertices is
// outside the guard band, its control word becomes 8.x, or 8.w with 13.z set (the triangle for
// the next program to clip) unless all three are outside one plane or the vertex starts a strip
// (its control word's bit 15). When the screen triangle winds backwards (the cross product of its
// edges, in the order that alternates along the strip, negative), 9.z's amount (16384, or 0) is
// added to w.
//
// The loop is pipelined four vertices deep, in four register sets that rotate (vertex m in set
// m % 4), with the four edges in vf24, vf29, vf14, vf19. When a vertex starting a strip closes a
// triangle wholly beyond the far plane, 0x274's two branches at 0x27A and 0x27B are both taken
// (the second in the first's delay slot): 0x28B runs twice and the return lands one instruction
// late, skipping that block's edge (the register keeps what it held) and making its backface add
// unconditional. The edges' registers start as the program before left them. What the loop leaves
// in the sets at its end (the vertices after the last, read ahead) is read by later programs, so
// it is rebuilt here as the loop leaves it.
constexpr int kSetP[4] = {15, 20, 25, 10}, kSetT[4] = {16, 21, 26, 11}, kSetF[4] = {17, 22, 27, 12};
constexpr int kEdge[4] = {24, 29, 14, 19};

struct LitVertex {
    V pos, stq;        // as loaded
    V p;               // clip coordinates
    V pClamped;        // w clamped to 8.z
    V pq, tq;          // times 1/w (tq.w the unclamped w)
    V f;               // the offset with the control word in w
    V s;               // the screen position
    std::uint32_t code;
    bool late;         // 0x274 returned one late (above)
};

bool LitBatchTransform(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V hdr = Load(r, top + 11);
    std::uint16_t first = (std::uint16_t)(top + Half(hdr.x));
    std::uint16_t end = (std::uint16_t)(top + Half(hdr.y));
    std::uint16_t span = (std::uint16_t)(end - first);
    // an empty batch or one not of whole vertices runs on past its end: left to the microcode
    constexpr int kMax = 256;
    if (span == 0 || (span & 3) || span / 4 > kMax)
        return false;
    const int n = span / 4;
    static LitVertex v[kMax + 4];

    V m[4] = {Load(r, top + 14), Load(r, top + 15), Load(r, top + 16), Load(r, top + 17)};
    V params = Load(r, top + 8);
    V scale = Load(r, 0), offset = Load(r, 1);
    scale.w = Mul(1.0f, params.y);
    V zero;   // vf09, and vf07 before its w
    zero.v = F4{Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(1.0f, 1.0f)};
    V backface = zero;
    if (Half(Load(r, top + 9).z) != 0)
        backface.w = Add(zero.w, 16384.0f);
    params.x = Add(params.x, 32768.0f);
    const float wOutside = Add(zero.w, params.x), wToClip = Add(zero.w, params.w);
    const std::uint16_t lastCheck = (std::uint16_t)(end - 4);

    // Every vertex the loop touches: the batch's n and the four after it it reads ahead.
    std::uint32_t codes = 0;   // FCSET 0, then each CLIP shifts in six bits
    for (int k = 0; k < n + 4; k++) {
        LitVertex& x = v[k];
        std::uint16_t at = (std::uint16_t)(first + 4 * k);
        x.pos = Load(r, at + 3);
        x.stq = Load(r, at);
        x.f = offset;
        x.f.w = x.pos.w;
        x.late = false;
        if (k > n + 2)
            continue;   // the last read ahead is only loaded
        x.p = Transform(m, x.pos);
        x.code = ClipCode(x.p);
        float q = Reciprocal(r, x.p.w);
        x.pClamped = x.p;
        x.pClamped.w = Mini(x.p.w, params.z);
        x.tq = x.stq;
        x.tq.w = x.p.w;
        if (k > n + 1)
            continue;   // its divide is not reached
        x.tq.v = Mul4(x.stq.v, Splat(q));
        x.tq.w = x.p.w;
        x.pq.v = Mul4(x.pClamped.v, Splat(q));
    }
    for (int k = 0; k < n + 3; k++) {
        LitVertex& x = v[k];
        codes = ((codes << 6) | x.code) & 0xFFFFFF;
        if (k > n + 1)
            continue;
        // the test of the triangle x closes, in the block two vertices before
        if (k >= 2 && (codes & 0x3FFFF)) {
            std::uint16_t control = Half(x.f.w);
            x.f.w = wOutside;
            std::uint16_t behind = (std::uint16_t)(first + 4 * (k - 1));   // the program's vi05 then
            bool ended = (std::int16_t)(std::uint16_t)(behind - lastCheck) >= 0;
            if (!ended) {
                bool beyondNear = (codes & 0x20820) == 0x20820, beyondFar = (codes & 0x10410) == 0x10410;
                if (!beyondNear && (control & 0x8000)) {
                    v[k - 2].late = beyondFar;
                } else if (!AllOutsideOnePlane(codes)) {
                    x.f.w = wToClip;
                    r.Qword(top + 13)[2] = FromBits(1);
                }
            }
        }
        x.s.v = Add4(Mul4(x.pq.v, scale.v), Mul4(x.f.v, Splat(1.0f)));
    }

    // The blocks: vertex k stored, the edge to k + 1, the backface of k by the cross product the
    // block before found.
    V edge[4];   // vf19 is vf00 first
    for (int e = 0; e < 3; e++)
        edge[e].v = F4{st.vf[kEdge[e]][0], st.vf[kEdge[e]][1], 0.0f, 0.0f};
    edge[3].v = F4{0.0f, 0.0f, 0.0f, 0.0f};
    bool negative = false;   // the status flag's sign as FSAND reads it (ADD vf00 first: none)
    for (int k = 0; k < n; k++) {
        LitVertex& x = v[k];
        V& cur = edge[k & 3];
        const V& prev = edge[(k + 3) & 3];
        if (!x.late) {
            cur.x = Sub(v[k + 1].s.x, x.s.x);
            cur.y = Sub(v[k + 1].s.y, x.s.y);
        }
        if (x.late || negative)
            x.s.w = Add(x.s.w, backface.w);
        float cross = (k & 1) ? Sub(Mul(cur.x, prev.y), Mul(prev.x, cur.y))
                              : Sub(Mul(prev.x, cur.y), Mul(cur.x, prev.y));
        negative = std::signbit(cross) && cross != 0.0f;
        std::uint16_t at = (std::uint16_t)(first + 4 * k);
        Store(r, at, x.tq);
        Store(r, at + 3, x.s);
    }

    // The registers as the loop leaves them after the block of vertex n - 1.
    auto set = [&](int reg, const V& x) { std::memcpy(st.vf[reg], &x, 16); };
    const int last = n - 1;
    for (int j = 1; j <= 4; j++) {
        const LitVertex& x = v[last + j];
        int s4 = (last + j) & 3;
        if (j <= 2) {
            set(kSetP[s4], x.pq);
            set(kSetT[s4], x.tq);
            set(kSetF[s4], x.f);
        } else if (j == 3) {
            set(kSetP[s4], x.pClamped);
            set(kSetT[s4], x.tq);
            set(kSetF[s4], x.f);
        } else {
            set(kSetP[s4], x.pos);
            set(kSetT[s4], x.stq);
            set(kSetF[s4], v[last].f);
        }
    }
    for (int e = 0; e < 4; e++) {
        st.vf[kEdge[e]][0] = edge[e].x;
        st.vf[kEdge[e]][1] = edge[e].y;
    }
    st.vf[19][2] = 0.0f;
    st.vf[19][3] = 1.0f;
    for (int c = 0; c < 4; c++) {
        set(2 + c, m[c]);
    }
    set(6, scale);
    set(7, backface);
    set(8, params);
    set(9, zero);
    st.clip = codes;
    st.pc = 0x3D4;
    return true;
}

// --- The batch's colours and kicks (chunk E1624411): 0x002 after the transform, 0x01E for a
// batch's second pass.

// ITOF0 and FTOI0, field by field as the runner does them.
inline float Itof0(float x) { return (float)(std::int32_t)Bits(x) / 1.0f; }
inline float Ftoi0(float x)
{
    double v = (double)In(x) * 1.0f;
    std::int32_t n = v >= 2147483647.0 ? 0x7FFFFFFF : v <= -2147483648.0 ? (std::int32_t)0x80000000 : (std::int32_t)v;
    return FromBits((std::uint32_t)n);
}

void SetVf(State& st, int reg, const V& x) { std::memcpy(st.vf[reg], &x, 16); }

// Whether x - 1 is zero (SUBw with vf00w; the Z flag FSAND reads four instructions later).
bool IsOne(float x) { return Sub(x, 1.0f) == 0.0f; }

// A loop that runs from first, step by step, while its pointer has not met end after the step
// (IBNE after the increment): the count, or 0 when it would run on past the end (wrapping).
int Count(std::uint16_t first, std::uint16_t end, int step)
{
    std::uint16_t span = (std::uint16_t)(end - first);
    if (span == 0 || step <= 0 || span % step)
        return 0;
    return span / step;
}

void Kick(Runner& r, std::uint32_t qaddr)
{
    if (r.ctx.kick)
        r.ctx.kick(qaddr & 0x3FF, r.ctx.user);
}

// 0x4C1: each vertex's colour (quadword 2) scaled by 10.w, or not when that is 1; quadword 1's w
// copied into its w. vf11.w, vf12 and vf13.w as the loop leaves them.
void Colours4C1(Runner& r, std::uint16_t top, int n)
{
    State& st = r.s;
    std::uint16_t first = (std::uint16_t)(top + Half(Load(r, top + 11).x));
    float scale = Load(r, top + 10).w;
    st.vf[11][3] = scale;
    bool one = IsOne(scale);
    for (int k = 0; k < n; k++) {
        std::uint16_t at = (std::uint16_t)(first + 4 * k);
        float* c = r.Qword(at + 2);
        float w = r.Qword(at + 1)[3];
        if (!one) {
            V x = Load(r, at + 2);
            for (int j = 0; j < 3; j++)
                x[j] = Ftoi0(Mul(Itof0(x[j]), scale));
            c[0] = x[0];
            c[1] = x[1];
            c[2] = x[2];
            if (k == n - 1)
                std::memcpy(st.vf[12], c, 12);
        }
        c[3] = w;
        if (k == n - 1)
            st.vf[13][3] = w;
    }
}

// 0x4DD: each vertex's quadword 1 xyz from the array at 12.w, scaled by 10.w unless that is 1.
void Colours4DD(Runner& r, std::uint16_t top, int n)
{
    State& st = r.s;
    std::uint16_t first = (std::uint16_t)(top + Half(Load(r, top + 11).x));
    std::uint16_t from = (std::uint16_t)(top + Half(Load(r, top + 12).w));
    float scale = Load(r, top + 10).w;
    st.vf[11][3] = scale;
    bool one = IsOne(scale);
    for (int k = 0; k < n; k++) {
        float x[3];
        std::memcpy(x, r.Qword(from + k), 12);
        if (!one)
            for (int j = 0; j < 3; j++)
                x[j] = Ftoi0(Mul(Itof0(x[j]), scale));
        std::memcpy(r.Qword(first + 4 * k + 1), x, 12);
        if (k == n - 1)
            std::memcpy(st.vf[12], x, 12);
    }
}

// The kick of a batch: the GIF tag at `tag` copied before the vertices (11.x), the packet after
// the tag kicked, then the vertices.
void KickBatch(Runner& r, std::uint16_t top, std::uint16_t tag)
{
    std::uint16_t at = (std::uint16_t)(top + tag);
    V t = Load(r, at);
    SetVf(r.s, 3, t);
    Kick(r, at + 1);
    std::uint16_t verts = (std::uint16_t)(top + Half(Load(r, top + 11).x));
    Store(r, verts - 1, t);
    Kick(r, verts - 1);
}

// Whether a batch can be done here: whole vertices, and no triangle flagged for the clipper.
bool Plain(Runner& r, std::uint16_t top, int& n)
{
    V hdr = Load(r, top + 11);
    n = Count((std::uint16_t)(top + Half(hdr.x)), (std::uint16_t)(top + Half(hdr.y)), 4);
    return n > 0 && Half(Load(r, top + 13).z) == 0;
}

// 0x01E's body from 0x020: the colours (0x4C1), the kick of the packet at 12.y, and when 9.w is
// not 1 the second colours (0x4DD) and the packet at 12.z.
void SecondPass(Runner& r, std::uint16_t top, int n)
{
    Colours4C1(r, top, n);
    KickBatch(r, top, Half(Load(r, top + 12).y));
    if (Half(Load(r, top + 9).w) == 1)
        return;
    Colours4DD(r, top, n);
    KickBatch(r, top, Half(Load(r, top + 12).z));
}

bool BatchSecond(Runner& r)
{
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V nine = Load(r, top + 9);
    r.s.pc = 0x051;
    if (Half(nine.y) != 0 || Half(nine.w) == 0)
        return true;
    int n;
    if (!Plain(r, top, n))
        return false;
    SecondPass(r, top, n);
    return true;
}

// 0x4A9: quadword 1 of each vertex (stride 13.x) scaled, xyz by 10.w and w by 9.x, unless both
// are 1.
bool Scale4A9(Runner& r, std::uint16_t top)
{
    State& st = r.s;
    float sw = Load(r, top + 9).x, sxyz = Load(r, top + 10).w;
    std::uint16_t first = (std::uint16_t)(top + Half(Load(r, top + 11).x));
    std::uint16_t end = (std::uint16_t)(top + Half(Load(r, top + 11).y));
    std::uint16_t step = Half(Load(r, top + 13).x);
    V factor;
    factor.v = F4{Add(0.0f, sxyz), Add(0.0f, sxyz), Add(0.0f, sxyz), Add(0.0f, sw)};
    st.vf[2][0] = factor.x;   // vf02: x from 9.x first, then all four
    SetVf(st, 2, factor);
    st.vf[3][3] = sxyz;
    std::memset(st.vf[5], 0, 16);
    if (IsOne(sw) && IsOne(sxyz))
        return true;
    // the loop stops when its pointer, before the step, is end - step
    std::uint16_t last = (std::uint16_t)(end - step);
    std::uint16_t span = (std::uint16_t)(last - first);
    if (step == 0 || span % step || span / step > 1024)
        return false;
    int count = span / step + 1;
    V x;
    for (int k = 0; k < count; k++) {
        std::uint16_t at = (std::uint16_t)(first + step * k + 1);
        x = Load(r, at);
        for (int j = 0; j < 4; j++)
            x[j] = Itof0(x[j]);
        x.v = Mul4(x.v, factor.v);
        for (int j = 0; j < 4; j++)
            x[j] = Ftoi0(x[j]);
        Store(r, at, x);
    }
    SetVf(st, 4, x);
    return true;
}

bool BatchFinish(Runner& r)
{
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    int n;
    if (!Plain(r, top, n))
        return false;
    V nine = Load(r, top + 9);
    bool second = Half(nine.y) != 0 && Half(nine.w) != 0;
    // a quadword-1 scaling the handler leaves to the microcode is decided before anything changes
    State saved = r.s;
    if (!Scale4A9(r, top)) {
        r.s = saved;
        return false;
    }
    KickBatch(r, top, Half(Load(r, top + 12).x));
    if (Half(nine.y) == 0) {
        r.s.pc = 0x01E;
        return true;
    }
    r.s.pc = 0x051;
    if (second)
        SecondPass(r, top, n);
    return true;
}

// --- 0x1A6 of chunk 6744EAB9: a point light on a batch's vertices.
//
// From TOP: 0-3 the matrix to world space, 22 the light's colour for the vertex colours, 23 and 24
// its scale and ambient, 25.xy the attenuation (a + b / distance), 26 its position; 11.x and 11.y
// the vertices, 12.w the colours (one quadword a vertex). Each vertex: the normal (quadword 0)
// turned and made unit length (ERLENG), the position (quadword 3) to world space, the light's
// direction from it; the cosine over the distance d, as d + d^3 (the z term weighted by vf01.x);
// the light (ambient + scale times that, clamped to 0..1) times the attenuation (clamped), added
// to quadword 2's xy and, through 22 by its z, to the colour. 9.y = 4 (another kind of light, at
// 0x142) is left to the microcode.

float Max(float a, float b) { return In(a) > In(b) ? In(a) : In(b); }

// ERLENG: 1 / the length of xyz, or the largest float for 0.
float Erleng(const V& v)
{
    float x = In(v.x), y = In(v.y), z = In(v.z);
    float sum2 = Add(Add(Mul(x, x), Mul(y, y)), Mul(z, z));
    return sum2 == 0.0f ? 3.4028235e38f : core::Fix(1.0f / std::sqrt(sum2));
}

void SetXyz(State& st, int reg, const V& x) { std::memcpy(st.vf[reg], &x, 12); }

bool PointLight(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    if (Half(Load(r, top + 9).y) == 4)
        return false;
    V hdr = Load(r, top + 11);
    std::uint16_t first = (std::uint16_t)(top + Half(hdr.x));
    int n = Count(first, (std::uint16_t)(top + Half(hdr.y)), 4);
    if (n == 0)
        return false;
    std::uint16_t colours = (std::uint16_t)(top + Half(Load(r, top + 12).w));
    V m[4] = {Load(r, top + 0), Load(r, top + 1), Load(r, top + 2), Load(r, top + 3)};
    V light = Load(r, top + 26), colour = Load(r, top + 22), ambient = Load(r, top + 24), scale = Load(r, top + 23);
    V atten = Load(r, top + 25);
    const float one = st.vf[1][0];   // vf01.x
    V nrm, pos, dir, lit, q2, col, prod;
    float p1 = 0, p2 = 0, cosine = 0, cube = 0, fade = 0;
    for (int k = 0; k < n; k++) {
        std::uint16_t at = (std::uint16_t)(first + 4 * k);
        nrm = Load(r, at);
        V p = Load(r, at + 3);
        pos = p;
        F4 acc = Mul4(m[0].v, Splat(nrm.x));
        acc = Add4(acc, Mul4(m[1].v, Splat(nrm.y)));
        V turned;
        turned.v = Add4(acc, Mul4(m[2].v, Splat(nrm.z)));
        acc = Mul4(m[0].v, Splat(p.x));
        acc = Add4(acc, Mul4(m[1].v, Splat(p.y)));
        acc = Add4(acc, Mul4(m[2].v, Splat(p.z)));
        pos.v = Add4(acc, Mul4(m[3].v, Splat(1.0f)));
        p1 = Erleng(turned);
        q2 = Load(r, at + 2);
        dir.v = Fix4(Fix4(light.v) - Fix4(pos.v));
        nrm.x = turned.x;
        nrm.y = turned.y;
        nrm.z = turned.z;
        V unit;
        unit.v = Mul4(turned.v, Splat(p1));
        nrm.x = unit.x;
        nrm.y = unit.y;
        nrm.z = unit.z;
        p2 = Erleng(dir);
        prod.v = Mul4(unit.v, dir.v);
        col = Load(r, (std::uint16_t)(colours + k));
        float a = Add(prod.x, prod.y);
        cosine = Add(a, Mul(one, prod.z));
        cosine = Mul(cosine, p2);
        cube = Mul(cosine, cosine);
        a = Add(cosine, 0.0f);
        cosine = Add(a, Mul(cube, cosine));
        acc = Add4(Splat(0.0f), ambient.v);
        lit.v = Add4(acc, Mul4(scale.v, Splat(cosine)));
        for (int c = 0; c < 3; c++)
            lit[c] = Mini(Max(lit[c], 0.0f), 1.0f);
        a = Add(0.0f, atten.x);
        fade = Add(a, Mul(p2, atten.y));
        fade = Mini(Max(fade, 0.0f), 1.0f);
        lit.v = Mul4(lit.v, Splat(fade));
        q2.x = Add(q2.x, lit.x);
        q2.y = Add(q2.y, lit.y);
        acc = Add4(Splat(0.0f), col.v);
        col.v = Add4(acc, Mul4(colour.v, Splat(lit.z)));
        float* out = r.Qword(at + 2);
        out[0] = q2.x;
        out[1] = q2.y;
        std::memcpy(r.Qword((std::uint16_t)(colours + k)), &col, 12);
    }
    // the registers as the loop leaves them
    SetXyz(st, 2, colour);
    SetXyz(st, 3, ambient);
    SetXyz(st, 4, scale);
    SetXyz(st, 5, light);
    st.vf[6][0] = atten.x;
    st.vf[6][1] = atten.y;
    for (int c = 0; c < 4; c++)
        SetVf(st, 7 + c, m[c]);
    SetXyz(st, 11, pos);
    SetVf(st, 12, nrm);
    SetXyz(st, 13, q2);
    SetXyz(st, 14, col);
    SetXyz(st, 15, dir);
    st.vf[16][0] = cosine;
    st.vf[16][1] = prod.y;
    st.vf[16][2] = prod.z;
    st.vf[17][0] = cube;
    st.vf[18][0] = p2;
    st.vf[19][0] = fade;
    SetXyz(st, 20, lit);
    st.p = p2;
    st.pc = 0x1DE;
    return true;
}

}  // namespace

const HandlerEntry kHandlers[] = {
    {0x79A6F5FB, 0x356, LitBatchTransform, "lit batch transform",
     "vf02.xyzw vf03.xyzw vf04.xyzw vf05.xyzw vf06.xyzw vf07.xyzw vf08.xyzw vf09.xyzw vf10.xyzw vf11.xyzw "
     "vf12.xyzw vf14.xy vf15.xyzw vf16.xyzw vf17.xyzw vf19.xyzw vf20.xyzw vf21.xyzw vf22.xyzw vf24.xy "
     "vf25.xyzw vf26.xyzw vf27.xyzw vf29.xy clip"},
    {0xE1624411, 0x002, BatchFinish, "lit batch finish", "vf02.xyzw vf03.xyzw vf04.xyzw vf05.xyzw vf11.w vf12.xyz vf13.w"},
    {0x6744EAB9, 0x1A6, PointLight, "point light",
     "vf02.xyz vf03.xyz vf04.xyz vf05.xyz vf06.xy vf07.xyzw vf08.xyzw vf09.xyzw vf10.xyzw vf11.xyz vf12.xyzw "
     "vf13.xyz vf14.xyz vf15.xyz vf16.xyz vf17.x vf18.x vf19.x vf20.xyz P"},
    {0xE1624411, 0x01E, BatchSecond, "lit batch second pass", "vf03.xyzw vf11.w vf12.xyz vf13.w"},
};
const std::size_t kHandlerCount = sizeof kHandlers / sizeof kHandlers[0];

}  // namespace vu1
}  // namespace port
