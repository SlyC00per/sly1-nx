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

}  // namespace

const HandlerEntry kHandlers[] = {
    {0x79A6F5FB, 0x356, LitBatchTransform, "lit batch transform",
     "vf02.xyzw vf03.xyzw vf04.xyzw vf05.xyzw vf06.xyzw vf07.xyzw vf08.xyzw vf09.xyzw vf10.xyzw vf11.xyzw "
     "vf12.xyzw vf14.xy vf15.xyzw vf16.xyzw vf17.xyzw vf19.xyzw vf20.xyzw vf21.xyzw vf22.xyzw vf24.xy "
     "vf25.xyzw vf26.xyzw vf27.xyzw vf29.xy clip"},
};
const std::size_t kHandlerCount = sizeof kHandlers / sizeof kHandlers[0];

}  // namespace vu1
}  // namespace port
