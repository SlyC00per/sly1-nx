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
#include "port/gs.h"
#include "port/system.h"
#include "port/gs_backend.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <map>
#include <vector>
#include <algorithm>

namespace port {
namespace vu1 {

namespace {

using core::Bits;
using core::FromBits;
using core::In;
using core::Runner;

// The VU's scalar arithmetic as core::Add, Sub and Mul, with the same reasoning as Mul4 below:
// only an operand with the largest exponent needs fixing on the render thread (denormals flushed,
// rounding toward zero).
inline float FixIn(float x)
{
    std::uint32_t b;
    std::memcpy(&b, &x, 4);
    if ((b & 0x7F800000u) == 0x7F800000u) {
        b = (b & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&x, &b, 4);
    }
    return x;
}
inline float Add(float a, float b) { return FixIn(a) + FixIn(b); }
inline float Sub(float a, float b) { return FixIn(a) - FixIn(b); }
inline float Mul(float a, float b) { return FixIn(a) * FixIn(b); }

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
// An operand with the largest exponent made the largest finite float of its sign. A denormal
// operand needs nothing: the thread flushes denormal operands and results to zero, keeping the
// sign (FPCR.FZ; FTZ and DAZ on x86), as the VU reads and writes them. Nor does the result: the
// thread rounds toward zero, so finite operands overflow to the largest finite float, as the VU's
// do, and never give an infinity or a NaN.
inline F4 FixIn4(F4 x)
{
    U4 b = (U4)x;
    U4 big = (U4)((b & 0x7F800000u) == 0x7F800000u);
    return (F4)((b & ~big) | (((b & 0x80000000u) | 0x7F7FFFFFu) & big));
}
inline F4 Mul4(F4 a, F4 b) { return FixIn4(a) * FixIn4(b); }
inline F4 Add4(F4 a, F4 b) { return FixIn4(a) + FixIn4(b); }
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
float Max(float a, float b) { return In(a) > In(b) ? In(a) : In(b); }

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

// The same loop at 0x29B (from 0x34A and 0x350) has vertices of three quadwords, the position in
// the last, and calls the test through vi03: 0x274 as above (0x34A) or 0x28E (0x350), which only
// leaves a triangle wholly beyond the far plane, or one a strip's start closes, unmarked.
enum class Test { Guard274, Far28E };

// The VU's arithmetic with its rules applied (E) or as the host does it: the game's thread
// rounds toward zero (so an overflow is the largest finite float, as on the VU) and flushes
// denormals to signed zeros (as the VU reads and writes them), so the two agree whenever no
// input has the largest exponent or is a denormal; a handler scans its inputs to choose.
template <bool E> inline F4 M4(F4 a, F4 b) { if (E) return Mul4(a, b); return a * b; }
template <bool E> inline F4 A4(F4 a, F4 b) { if (E) return Add4(a, b); return a + b; }
template <bool E> inline float M1(float a, float b) { if (E) return Mul(a, b); return a * b; }
template <bool E> inline float A1(float a, float b) { if (E) return Add(a, b); return a + b; }
template <bool E> inline float S1(float a, float b) { if (E) return Sub(a, b); return a - b; }
template <bool E> inline float Mini1(float a, float b) { if (E) return Mini(a, b); return a < b ? a : b; }
template <bool E> V TransformT(const V* m, const V& p)
{
    V o;
    F4 acc = M4<E>(m[0].v, Splat(p.x));
    acc = A4<E>(acc, M4<E>(m[1].v, Splat(p.y)));
    acc = A4<E>(acc, M4<E>(m[2].v, Splat(p.z)));
    o.v = A4<E>(acc, M4<E>(m[3].v, Splat(1.0f)));
    return o;
}
template <bool E> std::uint32_t ClipCodeT(const V& p)
{
    if (E)
        return ClipCode(p);
    float wv = std::fabs(p.w);
    std::uint32_t j = 0;
    for (int k = 0; k < 3; k++) {
        if (p[k] > wv)
            j |= 1u << (2 * k);
        if (p[k] < -wv)
            j |= 2u << (2 * k);
    }
    return j;
}

// Whether every field of the n quadwords at qaddr is an ordinary float (neither the largest
// exponent nor a denormal), so that the host's arithmetic gives the VU's results.
bool Ordinary(Runner& r, std::uint16_t qaddr, int n, int stride = 1)
{
    std::uint32_t bad = 0;
    for (int k = 0; k < n; k++) {
        const std::uint32_t* q = reinterpret_cast<const std::uint32_t*>(r.Qword((std::uint16_t)(qaddr + k * stride)));
        for (int c = 0; c < 4; c++) {
            std::uint32_t e = q[c] & 0x7F800000u;
            bad |= (e == 0x7F800000u) | (e == 0 && (q[c] & 0x007FFFFFu) != 0);
        }
    }
    return bad == 0;
}
constexpr int kEdgeRegs[3] = {24, 29, 14};

// Whether no field of the n quadwords at qaddr has the largest exponent: for data that only goes
// through the arithmetic (or is copied), where a denormal is flushed to zero by the thread as the VU
// reads it, so the host's arithmetic gives the VU's results (a batch's vertices).
bool FiniteFields(Runner& r, std::uint16_t qaddr)
{
    const std::uint32_t* q = reinterpret_cast<const std::uint32_t*>(r.Qword(qaddr));
    std::uint32_t bad = 0;
    for (int c = 0; c < 4; c++)
        bad |= (q[c] & 0x7F800000u) == 0x7F800000u;
    return bad == 0;
}

bool OrdinaryField(float f)
{
    std::uint32_t b = Bits(f), e = b & 0x7F800000u;
    return e != 0x7F800000u && !(e == 0 && (b & 0x007FFFFFu) != 0);
}

template <bool E>
bool BatchTransformT(Runner& r, const int stride, const Test test, const std::uint32_t endPc)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V hdr = Load(r, top + 11);
    std::uint16_t first = (std::uint16_t)(top + Half(hdr.x));
    std::uint16_t end = (std::uint16_t)(top + Half(hdr.y));
    std::uint16_t span = (std::uint16_t)(end - first);
    // an empty batch or one not of whole vertices runs on past its end: left to the microcode
    constexpr int kMax = 256;
    if (span == 0 || span % stride || span / stride > kMax)
        return false;
    const int n = span / stride;
    const int posAt = stride - 1;
    static LitVertex v[kMax + 4];

    V m[4] = {Load(r, top + 14), Load(r, top + 15), Load(r, top + 16), Load(r, top + 17)};
    V params = Load(r, top + 8);
    V scale = Load(r, 0), offset = Load(r, 1);
    scale.w = M1<E>(1.0f, params.y);
    V zero;   // vf09, and vf07 before its w
    zero.v = F4{S1<E>(0.0f, 0.0f), S1<E>(0.0f, 0.0f), S1<E>(0.0f, 0.0f), S1<E>(1.0f, 1.0f)};
    V backface = zero;
    if (Half(Load(r, top + 9).z) != 0)
        backface.w = A1<E>(zero.w, 16384.0f);
    params.x = A1<E>(params.x, 32768.0f);
    const float wOutside = A1<E>(zero.w, params.x), wToClip = A1<E>(zero.w, params.w);
    const std::uint16_t lastCheck = (std::uint16_t)(end - stride);

    // Every vertex the loop touches: the batch's n and the four after it it reads ahead.
    std::uint32_t codes = 0;   // FCSET 0, then each CLIP shifts in six bits
    for (int k = 0; k < n + 4; k++) {
        LitVertex& x = v[k];
        std::uint16_t at = (std::uint16_t)(first + stride * k);
        x.pos = Load(r, at + posAt);
        x.stq = Load(r, at);
        x.f = offset;
        x.f.w = x.pos.w;
        x.late = false;
        if (k > n + 2)
            continue;   // the last read ahead is only loaded
        x.p = TransformT<E>(m, x.pos);
        x.code = ClipCodeT<E>(x.p);
        float q = Reciprocal(r, x.p.w);
        x.pClamped = x.p;
        x.pClamped.w = Mini1<E>(x.p.w, params.z);
        x.tq = x.stq;
        x.tq.w = x.p.w;
        if (k > n + 1)
            continue;   // its divide is not reached
        x.tq.v = M4<E>(x.stq.v, Splat(q));
        x.tq.w = x.p.w;
        x.pq.v = M4<E>(x.pClamped.v, Splat(q));
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
            std::uint16_t behind = (std::uint16_t)(first + stride * (k - 1));   // the program's vi05 then
            bool ended = (std::int16_t)(std::uint16_t)(behind - lastCheck) >= 0;
            bool beyondNear = (codes & 0x20820) == 0x20820, beyondFar = (codes & 0x10410) == 0x10410;
            if (ended) {
            } else if (test == Test::Far28E) {
                if (!beyondFar && !(control & 0x8000)) {
                    x.f.w = wToClip;
                    r.Qword(top + 13)[2] = FromBits(1);
                }
            } else if (!beyondNear && (control & 0x8000)) {
                v[k - 2].late = beyondFar;
            } else if (!AllOutsideOnePlane(codes)) {
                x.f.w = wToClip;
                r.Qword(top + 13)[2] = FromBits(1);
            }
        }
        x.s.v = A4<E>(M4<E>(x.pq.v, scale.v), M4<E>(x.f.v, Splat(1.0f)));
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
            cur.x = S1<E>(v[k + 1].s.x, x.s.x);
            cur.y = S1<E>(v[k + 1].s.y, x.s.y);
        }
        if (x.late || negative)
            x.s.w = A1<E>(x.s.w, backface.w);
        float cross = (k & 1) ? S1<E>(M1<E>(cur.x, prev.y), M1<E>(prev.x, cur.y))
                              : S1<E>(M1<E>(prev.x, cur.y), M1<E>(cur.x, prev.y));
        negative = std::signbit(cross) && cross != 0.0f;
        std::uint16_t at = (std::uint16_t)(first + stride * k);
        Store(r, at, x.tq);
        Store(r, at + posAt, x.s);
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
    st.pc = endPc;
    return true;
}

void SetVf(State& st, int reg, const V& x);
void Kick(Runner& r, std::uint32_t qaddr);

// --- The batch's transform left to the GPU (port/gs_backend.h's Batch).
//
// With a renderer that takes batches, the transform does nothing but note itself: the vertices
// stay as VIF1 unpacked them, and each kick of the batch that follows (the finish's, the second
// pass's) hands them over with the matrix and the viewport instead of a packet. The renderer cuts
// triangles at the view's edges itself, so nothing is marked for the clipper. Should anything
// other than those kicks come next, or a kick the renderer does not take, the transform is done
// then, late, on the same data (Materialise). What the loop leaves in registers for a later
// program (the edges, the vertices read ahead) is then not what the microcode leaves: this way is
// for showing the game, not for SLY1_VU1_CHECK, which keeps it off.
struct Deferred {
    bool on = false, kicked = false;
    bool overwritten = false;      // VIF1 has unpacked over the batch's vertices since
    std::uint16_t first = 0, end = 0;   // the batch's quadwords
    V header[10], viewport[2];     // what the transform reads of TOP + 8 to 17 and of 0 and 1, as they were
    std::uint32_t serial = 0;      // counts the batches deferred (gs::Batch::id)
    std::uint16_t top = 0;
    int stride = 0;
    Test test = Test::Guard274;
    std::uint32_t endPc = 0;
} g_deferred;
std::map<std::uint64_t, std::uint64_t> g_lateBy;   // late transforms by program (hash << 12 | entry)
std::uint64_t g_lateNs, g_lateVertices, g_skippedOverwritten;

bool BatchTransformNow(Runner& r, const int stride, const Test test, const std::uint32_t endPc);

bool DeferredFor(std::uint16_t top) { return g_deferred.on && g_deferred.top == top; }
void DeferredKicked() { g_deferred.kicked = true; }

bool DeferTransforms()
{
    // SLY1_GPU_TRANSFORM: off unless asked for. With it some scenes of the opening lose their sky and
    // their lighting passes on the Switch's GPU and in the emulator (docs/RENDERER.md), though each
    // batch's arithmetic agrees with the program's (SLY1_GPU_BATCH_CHECK) and the host draws them right.
    static const bool on = std::getenv("SLY1_GPU_TRANSFORM") && !std::getenv("SLY1_VU1_CHECK");
    return on && gs::TakesBatches();
}

// The transform noted before, done now, in memory only: a program other than the batch's kicks may
// read what the transform leaves there (the outlines read the screen positions of a batch already
// drawn, 0x6DA those of the batch in the other half). Not when VIF1 has unpacked over the batch
// since: the memory is the next batch's.
void Materialise(Runner& r, bool always = false)
{
    if (!g_deferred.on)
        return;
    Deferred d = g_deferred;
    g_deferred.on = false;
    static const bool seq = std::getenv("SLY1_VU1_SEQ") != nullptr;
    if (seq)
        std::fprintf(stderr, "seq   materialise top %u (batch's top %u, overwritten %d)\n", r.ctx.top, d.top, (int)d.overwritten);
    if (d.overwritten) {
        g_skippedOverwritten++;
        return;
    }
    (void)always;
    // the batch's own half of the data memory (TOP), which need not be the program's now: a program
    // may read the batch the half before held (0x6DA does); the registers as they are
    {   // counted by the program it is done for, for the report (BatchCounts)
        std::uint32_t hash, entry;
        CurrentProgram(&hash, &entry);
        g_lateBy[(std::uint64_t)hash << 12 | entry]++;
        g_lateVertices += (std::uint16_t)(d.end - d.first) / (std::uint32_t)d.stride;
    }
    std::uint64_t lateStart = NowNs();
    struct LateTime {
        std::uint64_t start;
        ~LateTime() { g_lateNs += NowNs() - start; }
    } lateTime{lateStart};
    Context ctx = r.ctx;
    ctx.top = d.top;
    Runner late(ctx);
    State saved = r.s;
    // the header and the viewport as the batch had them (a later upload may have replaced them:
    // the sprites' own header, over a batch whose vertices they read), put back after
    V header[10], viewport[2];
    for (int k = 0; k < 10; k++) {
        header[k] = Load(late, (std::uint16_t)(d.top + 8 + k));
        Store(late, (std::uint16_t)(d.top + 8 + k), d.header[k]);
    }
    for (int k = 0; k < 2; k++) {
        viewport[k] = Load(late, (std::uint16_t)k);
        Store(late, (std::uint16_t)k, d.viewport[k]);
    }
    BatchTransformNow(late, d.stride, d.test, d.endPc);
    for (int k = 0; k < 10; k++)
        Store(late, (std::uint16_t)(d.top + 8 + k), header[k]);
    for (int k = 0; k < 2; k++)
        Store(late, (std::uint16_t)k, viewport[k]);
    r.s = saved;
}

// The viewport's offsets between 2 to the 33rd and 2 to the 34th, and its depths staying there:
// what makes the screen position's bits the GS's numbers (and the renderer's arithmetic the same).
bool ViewportAsExpected(Runner& r)
{
    V scale = Load(r, 0), offset = Load(r, 1);
    const float lo = 8589934592.0f, hi = 17179869184.0f;
    if (!(offset.x >= lo && offset.x < hi && offset.y >= lo && offset.y < hi && offset.z >= lo && offset.z < hi))
        return false;
    float depth = std::fabs(scale.z);
    return offset.z - depth >= lo && offset.z + depth < hi;
}

// A deferred batch's kick: the tag's packet as KickBatch sends it, then the batch itself to the
// renderer. False, with only the tag's packet sent, when the renderer does not take it.
bool KickDeferred(Runner& r, std::uint16_t top, std::uint16_t tag)
{
    const Deferred& d = g_deferred;
    std::uint16_t at = (std::uint16_t)(top + tag);
    V t = Load(r, at);
    V hdr = Load(r, top + 11);
    std::uint16_t first = (std::uint16_t)(top + Half(hdr.x)), end = (std::uint16_t)(top + Half(hdr.y));
    std::uint32_t count = (std::uint32_t)((std::uint16_t)(end - first) / d.stride);
    std::uint32_t word0 = Bits(t.x), word1 = Bits(t.y), nreg = (word1 >> 28) & 0xF, flg = (word1 >> 26) & 3;
    std::uint64_t regs = Bits(t.z) | (std::uint64_t)Bits(t.w) << 32;
    gs::Batch b;
    b.stride = (std::uint32_t)d.stride;
    b.count = count;
    b.positionAt = (std::uint32_t)d.stride - 1;
    int stqAt = -1, colourAt = -1;
    bool ok = flg == 0 && (int)nreg == d.stride && (word0 & 0x7FFF) == count && first + count * d.stride <= 1024 &&
              ((regs >> (4 * (nreg - 1))) & 0xF) == 4;
    for (std::uint32_t k = 0; ok && k + 1 < nreg; k++) {
        unsigned reg = (unsigned)(regs >> (4 * k)) & 0xF;
        if (reg == 2 && stqAt < 0)
            stqAt = (int)k;
        else if (reg == 1 && colourAt < 0)
            colourAt = (int)k;
        else if (reg != 0xF)
            ok = false;
    }
    ok = ok && stqAt >= 0 && colourAt >= 0;
    SetVf(r.s, 3, t);
    Kick(r, at + 1);
    Store(r, first - 1, t);
    if (!ok)
        return false;
    b.stqAt = (std::uint32_t)stqAt;
    b.colourAt = (std::uint32_t)colourAt;
    b.quads = reinterpret_cast<const std::uint32_t*>(r.Qword(first));
    for (int c = 0; c < 4; c++)
        std::memcpy(&b.matrix[c * 4], r.Qword((std::uint16_t)(top + 14 + c)), 16);
    std::memcpy(b.scale, r.Qword(0), 16);
    std::memcpy(b.offset, r.Qword(1), 16);
    V params = Load(r, top + 8);
    b.fogScale = params.y;
    b.wMax = params.z;
    b.cull = Half(Load(r, top + 9).z) != 0;
    b.id = g_deferred.overwritten ? 0 : g_deferred.serial;
    static const bool check = std::getenv("SLY1_GPU_BATCH_CHECK") != nullptr;
    if (check) {
        // SLY1_GPU_BATCH_CHECK: the renderer's arithmetic on the vertices as they are, against what
        // the program makes of them (done now, and the batch then kicked as a packet): the GS's
        // x, y, depth and fog of each vertex inside the view.
        static std::vector<std::uint32_t> raw;
        static std::uint64_t vertices, far;
        static double worst[4];
        raw.assign(b.quads, b.quads + count * b.stride * 4);
        Materialise(r, true);
        gs::Batch c = b;
        c.quads = raw.data();
        for (std::uint32_t k = 0; k < count; k++) {
            gs::BatchVertex v = gs::VertexOf(c, k);
            float w = v.clip[3];
            if (!(w > 0.0f && std::fabs(v.clip[0]) <= w && std::fabs(v.clip[1]) <= w && std::fabs(v.clip[2]) <= w))
                continue;
            const std::uint32_t* out = b.quads + (k * b.stride + b.positionAt) * 4;
            double d[4] = {std::fabs(v.x - (double)(out[0] & 0xFFFF)), std::fabs(v.y - (double)(out[1] & 0xFFFF)),
                           std::fabs(v.z - (double)((out[2] >> 4) & 0xFFFFFF)), std::fabs(v.fog - (double)((out[3] >> 4) & 0xFF))};
            vertices++;
            if (d[0] > 2 || d[1] > 2 || d[2] > 64 || d[3] > 1)
                far++;
            for (int j = 0; j < 4; j++)
                worst[j] = std::max(worst[j], d[j]);
        }
        static std::uint64_t batches;
        batches++;
        if ((batches & (batches - 1)) == 0)
            std::fprintf(stderr, "batch check: %llu batches, %llu vertices in view, %llu further than 2/16 pixel, 64 of depth "
                         "or 1 of fog; the furthest %.2f %.2f (x, y in sixteenths), %.1f (depth of 16,777,216), %.0f (fog)\n",
                         (unsigned long long)batches, (unsigned long long)vertices, (unsigned long long)far, worst[0], worst[1],
                         worst[2], worst[3]);
        return false;
    }
    return gs::DrawBatch(word1, b);
}

// What the transform leaves in registers for later programs (the matrix, the viewport, the last
// vertices' sets, the edges, the clip codes), without the batch transformed: the program's loop
// over the batch's last four to seven vertices alone (a multiple of four fewer keeps each vertex in the
// register set and on the side of the strip it has in the whole batch), their memory and the
// batch's header put back afterwards.
void LeaveRegisters(Runner& r, std::uint16_t top, int stride, Test test, std::uint32_t endPc)
{
    V hdr = Load(r, top + 11);
    std::uint16_t first = (std::uint16_t)(top + Half(hdr.x)), end = (std::uint16_t)(top + Half(hdr.y));
    int n = (std::uint16_t)(end - first) / stride;
    int tail = n > 7 ? 4 + (n & 3) : n;   // the same count modulo 4, so the same sets and sides
    std::uint16_t from = (std::uint16_t)(end - tail * stride);
    V saved[12 * 4], mark = Load(r, top + 13);
    for (int k = 0; k < tail * stride; k++)
        saved[k] = Load(r, (std::uint16_t)(from + k));
    V h = hdr;
    h.x = FromBits((Bits(hdr.x) & 0xFFFF0000u) | (std::uint16_t)(from - top));
    Store(r, top + 11, h);
    BatchTransformNow(r, stride, test, endPc);
    Store(r, top + 11, hdr);
    Store(r, top + 13, mark);
    for (int k = 0; k < tail * stride; k++)
        Store(r, (std::uint16_t)(from + k), saved[k]);
}

bool BatchTransform(Runner& r, const int stride, const Test test, const std::uint32_t endPc)
{
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    // (the far-plane test's batches stay the program's: it draws what lies wholly beyond the far
    // plane uncut, the sky, which a renderer cutting at the far plane would leave out)
    bool kept = false;   // one of a frame's first batches a diagnosis keeps for the program
    if (int keep = gs::FirstBatchesKept()) {
        static std::uint64_t frame;
        static int count;
        if (gs::FramesShown() != frame) {
            frame = gs::FramesShown();
            count = 0;
        }
        kept = count++ < keep;
    }
    if (test == Test::Guard274 && !kept && DeferTransforms()) {
        V hdr = Load(r, top + 11);
        std::uint16_t span = (std::uint16_t)(Half(hdr.y) - Half(hdr.x));
        if (span != 0 && span % stride == 0 && span / stride <= 256 && Half(Load(r, top + 13).z) == 0 &&
            ViewportAsExpected(r)) {
            static const bool seqTrace = std::getenv("SLY1_VU1_SEQ") != nullptr;
            if (seqTrace)
                std::fprintf(stderr, "seq   deferred top %u first %u end %u stride %d%s\n", top, top + Half(hdr.x), top + Half(hdr.y), stride,
                             g_deferred.on ? " (the one before never done)" : "");
            g_deferred.on = true;
            g_deferred.serial++;
            g_deferred.kicked = false;
            g_deferred.overwritten = false;
            g_deferred.first = (std::uint16_t)(top + Half(hdr.x));
            g_deferred.end = (std::uint16_t)(top + Half(hdr.y));
            for (int k = 0; k < 10; k++)
                g_deferred.header[k] = Load(r, (std::uint16_t)(top + 8 + k));
            for (int k = 0; k < 2; k++)
                g_deferred.viewport[k] = Load(r, (std::uint16_t)k);
            g_deferred.top = top;
            g_deferred.stride = stride;
            g_deferred.test = test;
            g_deferred.endPc = endPc;
            static const bool leaveCheck = std::getenv("SLY1_GPU_LEAVE_CHECK") != nullptr;
            State full;
            static std::vector<std::uint8_t> memory;
            if (leaveCheck) {   // the registers the whole transform leaves, on copies, against LeaveRegisters'
                memory.assign(r.data, r.data + 16384);
                State before = r.s;
                BatchTransformNow(r, stride, test, endPc);
                full = r.s;
                r.s = before;
                std::memcpy(r.data, memory.data(), 16384);
            }
            static const bool noLeave = std::getenv("SLY1_NO_LEAVE") != nullptr;   // diagnosis: the registers left as they are
            if (noLeave) {
                r.s.clip = 0;
                r.s.pc = endPc;
            } else {
                LeaveRegisters(r, top, stride, test, endPc);
            }
            if (leaveCheck) {
                static std::uint64_t checks, differed;
                static std::uint32_t regsDiffer[32];
                static std::uint64_t otherDiffer[8];
                checks++;
                bool any = false;
                for (int k = 0; k < 32; k++)
                    if (std::memcmp(full.vf[k], r.s.vf[k], 16) != 0) {
                        regsDiffer[k]++;
                        any = true;
                    }
                const bool o[8] = {std::memcmp(full.vi, r.s.vi, sizeof full.vi) != 0, std::memcmp(full.acc, r.s.acc, 16) != 0,
                                   full.q != r.s.q, full.p != r.s.p, full.i != r.s.i || full.r != r.s.r, full.mac != r.s.mac,
                                   full.status != r.s.status, full.clip != r.s.clip};
                for (int k = 0; k < 8; k++)
                    if (o[k]) {
                        otherDiffer[k]++;
                        any = true;
                    }
                if (any)
                    differed++;
                if ((checks & (checks - 1)) == 0 || checks % 100000 == 0) {
                    std::fprintf(stderr, "leave check: %llu batches, %llu differed; registers:", (unsigned long long)checks,
                                 (unsigned long long)differed);
                    for (int k = 0; k < 32; k++)
                        if (regsDiffer[k])
                            std::fprintf(stderr, " vf%02d %u", k, regsDiffer[k]);
                    static const char* kOther[8] = {"vi", "acc", "Q", "P", "I/R", "MAC", "status", "clip"};
                    for (int k = 0; k < 8; k++)
                        if (otherDiffer[k])
                            std::fprintf(stderr, " %s %llu", kOther[k], (unsigned long long)otherDiffer[k]);
                    std::fprintf(stderr, "\n");
                }
            }
            return true;
        }
    }
    return BatchTransformNow(r, stride, test, endPc);
}

bool BatchTransformNow(Runner& r, const int stride, const Test test, const std::uint32_t endPc)
{
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V hdr = Load(r, top + 11);
    std::uint16_t first = (std::uint16_t)(top + Half(hdr.x));
    std::uint16_t span = (std::uint16_t)(Half(hdr.y) - Half(hdr.x));
    bool plain = span != 0 && span % stride == 0 && span / stride <= 256 && Ordinary(r, top + 14, 4) &&
                 Ordinary(r, top + 8, 1) && Ordinary(r, 0, 2);
    if (plain) {
        int n = span / stride;
        for (int k = 0; k < n + 4 && plain; k++) {
            std::uint16_t at = (std::uint16_t)(first + stride * k);
            plain = FiniteFields(r, at) && FiniteFields(r, (std::uint16_t)(at + stride - 1));
        }
        for (int e : kEdgeRegs)
            plain = plain && OrdinaryField(r.s.vf[e][0]) && OrdinaryField(r.s.vf[e][1]);
    }
    return plain ? BatchTransformT<false>(r, stride, test, endPc) : BatchTransformT<true>(r, stride, test, endPc);
}

bool LitBatchTransform(Runner& r) { return BatchTransform(r, 4, Test::Guard274, 0x3D4); }
bool BatchTransform274(Runner& r) { return BatchTransform(r, 3, Test::Guard274, 0x350); }
bool BatchTransform28E(Runner& r) { return BatchTransform(r, 3, Test::Far28E, 0x356); }

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

// ITOF0 and FTOI0 four fields at a time, by the same rules: the integer converted in the thread's
// rounding (toward zero); the float as the VU reads it (In), truncated, past the 32-bit range the
// largest integer of its sign.
inline F4 Itof4(F4 x) { return __builtin_convertvector((I4)x, F4); }
inline F4 Ftoi4(F4 x)
{
    x = Fix4(x);
    const F4 hi = Splat(2147483648.0f), lo = Splat(-2147483648.0f);
    I4 over = (I4)(x >= hi), under = (I4)(x < lo);
    F4 safe = (F4)(((U4)x & ~(U4)(over | under)));   // out-of-range fields made 0 before converting
    I4 n = __builtin_convertvector(safe, I4);
    n = (n & ~over) | ((I4)Splat(0.0f) + 0x7FFFFFFF & over);
    n = (n & ~under) | ((I4){(std::int32_t)0x80000000, (std::int32_t)0x80000000, (std::int32_t)0x80000000, (std::int32_t)0x80000000} & under);
    return (F4)n;
}

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
            x.v = Ftoi4(Mul4(Itof4(x.v), Splat(scale)));
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
        V v = Load(r, (std::uint16_t)(from + k));
        if (!one)
            v.v = Ftoi4(Mul4(Itof4(v.v), Splat(scale)));
        float x[3] = {v[0], v[1], v[2]};
        std::memcpy(r.Qword(first + 4 * k + 1), x, 12);
        if (k == n - 1)
            std::memcpy(st.vf[12], x, 12);
    }
}

// The kick of a batch: the GIF tag at `tag` copied before the vertices (11.x), the packet after
// the tag kicked, then the vertices.
bool KickDeferred(Runner& r, std::uint16_t top, std::uint16_t tag);
void Materialise(Runner& r, bool always);
std::uint64_t g_notTaken, g_taken;
bool DeferredFor(std::uint16_t top);
void DeferredKicked();

void KickBatch(Runner& r, std::uint16_t top, std::uint16_t tag)
{
    if (DeferredFor(top)) {   // the batch's transform is the renderer's
        if (KickDeferred(r, top, tag)) {
            DeferredKicked();
            g_taken++;
            return;
        }
        // not taken: the transform now, then the vertices' packet (the tag's is sent)
        g_notTaken++;
        Materialise(r, true);
        Kick(r, (std::uint16_t)(top + Half(Load(r, top + 11).x)) - 1);
        return;
    }
    std::uint16_t at = (std::uint16_t)(top + tag);
    V t = Load(r, at);
    SetVf(r.s, 3, t);
    Kick(r, at + 1);
    std::uint16_t verts = (std::uint16_t)(top + Half(Load(r, top + 11).x));
    Store(r, verts - 1, t);
    Kick(r, verts - 1);
}

// --- The clipper (chunk E1624411's 0x5EF with 0x54D, 0x545, 0x507, 0x5D2 and 0x5E5), for a batch
// whose transform marked triangles to clip (13.z).
//
// For each vertex from the third whose control word marks its triangle (bit 13 of the control
// word plus 4096), the triangle's three vertices (STQ, the colour at quadword vi03, the position)
// are copied to the work area at 13 of data memory and taken back to clip space (0x54D: the
// screen position less the viewport's offset, over its scale, times w; STQ times w), with codes
// against a guard band 6.34 by 18.1 times the screen. A triangle wholly outside one side of it
// is dropped; then the polygon is clipped against the planes w - z, w + z, w + y, w - y, w + x,
// w - x in turn (0x507: Sutherland-Hodgman in place, colours interpolated with w unless an end has
// w below 1), each only when CLIP's codes (of the polygon's last four vertices: after the first
// plane they are computed again once, then kept) put a vertex beyond it. The polygon is projected
// as 0x356 projects, culled by its signed area against the strip's winding (0x5D2, when 9.z asks),
// and kicked from the tag at 12 (its count with EOP), followed by an empty packet at 2; 13.z is
// set again for the next pass. The flags read are those of the operations four instructions
// before (five for the plane loop's MAC test), as the microcode is scheduled.
struct Clipper {
    Runner& r;
    State& st;
    std::uint16_t top;
    float (*vf)[4];
    explicit Clipper(Runner& run) : r(run), st(run.s), top((std::uint16_t)run.ctx.top), vf(run.s.vf) {}

    V g(int k) const { V x = {}; std::memcpy(&x, vf[k], 16); return x; }
    void p(int k, const V& x, int mask = 15)
    {
        for (int c = 0; c < 4; c++)
            if (mask & (8 >> c))
                vf[k][c] = x[c];
    }
    void lq(int k, std::uint16_t a, int mask = 15) { p(k, Load(r, a), mask); }
    void sq(int k, std::uint16_t a, int mask = 15)
    {
        V m = Load(r, a), x = g(k);
        for (int c = 0; c < 4; c++)
            if (mask & (8 >> c))
                m[c] = x[c];
        Store(r, a, m);
    }
    void Clip(const V& v) { st.clip = ((st.clip << 6) | ClipCode(v)) & 0xFFFFFF; }
    bool Fcor(std::uint32_t mask) const { return ((st.clip | mask) & 0xFFFFFF) == 0xFFFFFF; }
    bool Fcand(std::uint32_t mask) const { return (st.clip & mask) != 0; }
    static bool Negative(float x) { return std::signbit(x) && x != 0.0f; }
    // DIV Q, fs, ft
    float Div(float fs, float ft)
    {
        float a = In(fs), b = In(ft);
        if (b == 0.0f) {
            st.status |= a == 0.0f ? 0x410 : 0x820;
            return (std::signbit(a) != std::signbit(b)) ? -3.4028235e38f : 3.4028235e38f;
        }
        return core::Fix(a / b);
    }

    std::uint16_t vi06 = 13, vi07 = 22, vi10 = 0;

    // 0x545: the codes of the polygon's vertices against their w.
    void Codes()
    {
        std::uint16_t a = vi06;
        do {
            lq(9, (std::uint16_t)(a + 2));
            Clip(g(9));
            a = (std::uint16_t)(a + 3);
        } while (a != vi07);
    }

    // 0x507: the polygon clipped against the plane in vf06, in place.
    void Plane()
    {
        const float one = vf[1][0];
        vi10 = 1;
        vf[16][3] = FromBits(1);
        lq(13, (std::uint16_t)(vi06 + 2));
        auto dist = [&]() {
            V x;
            x.v = Mul4(g(6).v, g(13).v);
            p(14, x);
            float acc = Add(vf[14][0], vf[14][1]);
            acc = Add(acc, Mul(one, vf[14][2]));
            vf[14][0] = Add(acc, Mul(one, vf[14][3]));
        };
        dist();
        lq(12, (std::uint16_t)(vi06 + 1));
        lq(11, vi06);
        std::uint16_t vi12 = vi06, vi02 = vi06;
        auto weight = [&]() {
            V x;
            x.v = Mul4(g(12).v, Splat(vf[13][3]));
            p(20, x);
        };
        weight();
        bool vi11 = Negative(vf[14][0]);
        sq(11, vi07);
        sq(12, (std::uint16_t)(vi07 + 1));
        sq(13, (std::uint16_t)(vi07 + 2));
        do {
            V x;
            x.v = Add4(g(13).v, Splat(0.0f));
            p(9, x);
            bool vi10b = vi11;
            x.v = Add4(g(14).v, Splat(0.0f));
            p(10, x);
            p(7, g(11));
            x.v = Add4(g(20).v, Splat(0.0f));
            p(19, x);
            p(8, g(12));
            lq(13, (std::uint16_t)(vi12 + 5));
            {
                V d;
                d.v = Mul4(g(6).v, g(13).v);
                p(14, d);
            }
            lq(12, (std::uint16_t)(vi12 + 4));
            float acc = Add(vf[14][0], vf[14][1]);
            lq(11, (std::uint16_t)(vi12 + 3));
            acc = Add(acc, Mul(one, vf[14][2]));
            vf[21][2] = vf[9][3];
            vf[14][0] = Add(acc, Mul(one, vf[14][3]));
            vf[21][3] = vf[13][3];
            weight();
            vi11 = Negative(vf[14][0]);
            vf[15][0] = Sub(vf[14][0], vf[10][0]);
            vf[21][2] = Sub(vf[21][2], 1.0f);
            vf[21][3] = Sub(vf[21][3], 1.0f);
            if (vi10b != vi11) {
                float t = Div(vf[14][0], vf[15][0]);
                st.q = t;
                bool lowBefore = Negative(vf[21][2]);
                bool low = lowBefore || Negative(vf[21][3]);
                p(17, g(8));
                if (lowBefore)
                    p(17, g(12));
                vf[15][3] = Sub(1.0f, t);
                V n;
                n.v = Add4(Mul4(g(9).v, Splat(t)), Mul4(g(13).v, Splat(vf[15][3])));
                p(18, n);
                V s;
                s.v = Add4(Mul4(g(7).v, Splat(t)), Mul4(g(11).v, Splat(vf[15][3])));
                p(16, s, 14);
                if (!low) {
                    F4 c = Mul4(g(19).v, Splat(t));
                    st.q = Reciprocal(r, vf[18][3]);
                    V col;
                    col.v = Add4(c, Mul4(g(20).v, Splat(vf[15][3])));
                    col.v = Mul4(col.v, Splat(st.q));
                    p(17, col);
                }
                sq(18, (std::uint16_t)(vi02 + 2));
                sq(16, vi02);
                sq(17, (std::uint16_t)(vi02 + 1));
                vi02 = (std::uint16_t)(vi02 + 3);
            }
            if (!vi11) {
                sq(11, vi02);
                sq(12, (std::uint16_t)(vi02 + 1));
                sq(13, (std::uint16_t)(vi02 + 2));
                vi02 = (std::uint16_t)(vi02 + 3);
            }
            vi12 = (std::uint16_t)(vi12 + 3);
        } while (vi12 != vi07);
        vi07 = vi02;
    }

    // 0x54D: the triangle at the work area to clip space, tested, clipped and projected; vi10 the
    // vertices left (0 when dropped).
    void Polygon()
    {
        V zero;
        zero.v = F4{Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(1.0f, 1.0f)};
        p(7, zero);
        lq(5, (std::uint16_t)(top + 8));
        lq(2, 1);
        lq(3, 0);
        vf[2][3] = Mul(1.0f, vf[5][0]);
        vf[3][3] = Mul(1.0f, vf[5][1]);
        for (int c = 0; c < 4; c++)
            vf[11][c] = Max(c == 3 ? 1.0f : 0.0f, 1.0f);
        st.i = FromBits(0x40CAE148u);
        vf[11][0] = Add(0.0f, st.i);
        st.i = FromBits(0x4190CCCDu);
        vf[11][1] = Add(0.0f, st.i);
        for (int c = 0; c < 3; c++) {
            st.q = Reciprocal(r, vf[3][c]);
            vf[4][c] = Add(0.0f, st.q);
        }
        st.clip = 0;
        std::uint16_t a = vi06;
        do {
            lq(9, (std::uint16_t)(a + 2));
            lq(7, a);
            lq(8, (std::uint16_t)(a + 1));
            V old7 = g(7), v9 = g(9);
            for (int c = 0; c < 3; c++)
                v9[c] = Sub(v9[c], vf[2][c]);
            for (int c = 0; c < 3; c++)
                vf[7][c] = Mul(old7[c], old7.w);
            v9.w = old7.w;
            p(9, v9);
            vf[7][3] = Sub(1.0f, 1.0f);
            for (int c = 0; c < 4; c++)
                vf[8][c] = Itof0(vf[8][c]);
            a = (std::uint16_t)(a + 3);
            for (int c = 0; c < 3; c++)
                vf[9][c] = Mul(vf[9][c], vf[4][c]);
            for (int c = 0; c < 3; c++)
                vf[9][c] = Mul(vf[9][c], vf[9][3]);
            V v12;
            v12.v = Mul4(g(11).v, g(9).v);
            p(12, v12);
            sq(9, (std::uint16_t)(a - 1));
            Clip(v12);
            sq(8, (std::uint16_t)(a - 2));
            sq(7, (std::uint16_t)(a - 3));
        } while (a != vi07);
        // the triangle off one side of the guard band (vi03 is 0 here)
        if (Fcor(0xFF7DF7) || Fcor(0xFFBEFB) || Fcor(0xFFDF7D) || Fcor(0xFFEFBE)) {
            vi10 = 0;
            vi07 = vi06;
            return;
        }
        st.clip = 0;
        Codes();
        V base;
        base.v = F4{Add(0.0f, 0.0f), Add(0.0f, 0.0f), Add(0.0f, 0.0f), Add(1.0f, 0.0f)};
        p(6, base);
        if (Fcand(0x410410)) {
            vf[6][2] = Sub(vf[6][2], 1.0f);
            Plane();
            if (vi06 == vi07) {
                vi10 = 0;
                vi07 = vi06;
                return;
            }
            st.clip = 0;
            Codes();
        }
        struct { std::uint32_t mask; int field; bool add; } planes[5] = {
            {0x820820, 2, true}, {0x208208, 1, true}, {0x104104, 1, false}, {0x082082, 0, true}, {0x041041, 0, false}};
        for (const auto& pl : planes) {
            p(6, base);
            if (!Fcand(pl.mask))
                continue;
            vf[6][pl.field] = pl.add ? Add(vf[6][pl.field], 1.0f) : Sub(vf[6][pl.field], 1.0f);
            Plane();
            if (vi06 == vi07) {
                vi10 = 0;
                vi07 = vi06;
                return;
            }
        }
        // 0x5B8: projected as the transform projects
        vi10 = 0;
        a = vi06;
        do {
            lq(7, a);
            lq(8, (std::uint16_t)(a + 1));
            lq(9, (std::uint16_t)(a + 2));
            float q = Reciprocal(r, vf[9][3]);
            st.q = q;
            vf[9][3] = Mini(vf[9][3], vf[5][2]);
            for (int c = 0; c < 3; c++)
                vf[7][c] = Mul(vf[7][c], q);
            V v9;
            v9.v = Mul4(g(9).v, Splat(q));
            v9.v = Add4(Mul4(v9.v, g(3).v), Mul4(g(2).v, Splat(1.0f)));
            p(9, v9);
            st.i = 0.5f;
            V v8;
            v8.v = Add4(g(8).v, Splat(st.i));
            for (int c = 0; c < 4; c++)
                v8[c] = Ftoi0(v8[c]);
            p(8, v8);
            sq(7, a);
            sq(8, (std::uint16_t)(a + 1));
            sq(9, (std::uint16_t)(a + 2));
            a = (std::uint16_t)(a + 3);
            vi10 = (std::uint16_t)(vi10 + 1);
        } while (a != vi07);
    }

    // 0x5D2: the polygon culled by its winding when 9.z asks.
    void Cull(std::uint16_t vi05)
    {
        float acc = Sub(0.0f, 0.0f);
        std::uint16_t enabled = Half(Load(r, (std::uint16_t)(top + 9)).z);
        lq(10, (std::uint16_t)(vi06 + 2));
        lq(11, (std::uint16_t)(vi06 + 5));
        lq(12, (std::uint16_t)(vi06 + 8));
        vf[13][0] = Sub(vf[11][0], vf[10][0]);
        vf[13][1] = Sub(vf[11][1], vf[10][1]);
        std::uint16_t vi11 = (std::uint16_t)(vi06 + 6);
        if (enabled == 0)
            return;
        do {
            vf[14][0] = Sub(vf[12][0], vf[10][0]);
            vf[14][1] = Sub(vf[12][1], vf[10][1]);
            vi11 = (std::uint16_t)(vi11 + 3);
            acc = Add(acc, Mul(vf[14][0], vf[13][1]));
            lq(12, (std::uint16_t)(vi11 + 2));
            acc = Sub(acc, Mul(vf[13][0], vf[14][1]));
            p(13, g(14));
        } while (vi11 != vi07);
        std::uint16_t vi01 = Negative(acc) ? 2 : 0, vi02 = vi05 & 2;
        if (vi01 == vi02)
            vi07 = vi06;
    }

    // 0x671 (vi03 = 0): the same for vertices of three quadwords (STQ, colour, position), with
    // 15 noted at 34.x and 13.z left alone; 0x627, called last, returns at once, zeroing vf04 in
    // its delay slot.
    void Run3()
    {
        V hdr = Load(r, (std::uint16_t)(top + 11));
        std::uint16_t vi08 = (std::uint16_t)(top + Half(hdr.x) + 6), vi09 = (std::uint16_t)(top + Half(hdr.y));
        r.Qword((std::uint16_t)(top + 34))[0] = FromBits((std::uint16_t)(top + 15));
        std::uint16_t vi05 = 2;
        do {
            std::uint16_t control = Half(Load(r, (std::uint16_t)(vi08 + 2)).w);
            if ((std::uint16_t)(control + 4096) & 8192) {
                vi06 = 13;
                vi07 = 22;
                std::uint16_t vi11 = vi06, a = (std::uint16_t)(vi08 - 9);
                do {
                    a = (std::uint16_t)(a + 3);
                    lq(2, a);
                    lq(3, (std::uint16_t)(a + 1));
                    lq(4, (std::uint16_t)(a + 2));
                    sq(2, vi11);
                    sq(3, (std::uint16_t)(vi11 + 1));
                    sq(4, (std::uint16_t)(vi11 + 2));
                    vi11 = (std::uint16_t)(vi11 + 3);
                } while (vi11 != vi07);
                Polygon();
                if (vi06 != vi07) {
                    Cull(vi05);
                    if (vi06 != vi07) {
                        r.Qword(12)[0] = FromBits((std::uint16_t)(32768 + vi10));
                        Kick(r, 12);
                        float* nop = r.Qword(2);
                        nop[1] = 0.0f;
                        nop[2] = 0.0f;
                        nop[3] = 1.0f;
                        nop[0] = FromBits(32768);
                        Kick(r, 2);
                    }
                }
            }
            vi08 = (std::uint16_t)(vi08 + 3);
            vi05 = (std::uint16_t)(vi05 + 2);
        } while (vi08 != vi09);
        V zero;
        zero.v = F4{Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(1.0f, 1.0f)};
        p(4, zero);
    }

    // 0x5EF with the colour at quadword colour of each vertex.
    void Run(std::uint16_t colour)
    {
        r.Qword((std::uint16_t)(top + 13))[2] = FromBits(0);
        V hdr = Load(r, (std::uint16_t)(top + 11));
        std::uint16_t vi08 = (std::uint16_t)(top + Half(hdr.x) + 8), vi09 = (std::uint16_t)(top + Half(hdr.y));
        vf[30][0] = FromBits((std::uint32_t)(std::int32_t)(std::int16_t)colour);
        std::uint16_t vi05 = 2;
        do {
            std::uint16_t control = Half(Load(r, (std::uint16_t)(vi08 + 3)).w);
            if ((std::uint16_t)(control + 4096) & 8192) {
                vi06 = 13;
                vi07 = 22;
                std::uint16_t vi11 = vi06, a = (std::uint16_t)(vi08 - 12);
                do {
                    a = (std::uint16_t)(a + 4);
                    lq(2, a);
                    lq(3, (std::uint16_t)(a + colour));
                    lq(4, (std::uint16_t)(a + 3));
                    sq(2, vi11);
                    sq(3, (std::uint16_t)(vi11 + 1));
                    sq(4, (std::uint16_t)(vi11 + 2));
                    vi11 = (std::uint16_t)(vi11 + 3);
                } while (vi11 != vi07);
                Polygon();
                if (vi10 != 0) {
                    Cull(vi05);
                    if (vi06 != vi07) {
                        r.Qword(12)[0] = FromBits((std::uint16_t)(32768 + vi10));
                        r.Qword((std::uint16_t)(top + 13))[2] = FromBits(1);
                        Kick(r, 12);
                        float* nop = r.Qword(2);
                        nop[1] = 0.0f;
                        nop[2] = 0.0f;
                        nop[3] = 1.0f;
                        nop[0] = FromBits(32768);
                        Kick(r, 2);
                    }
                }
            }
            vi08 = (std::uint16_t)(vi08 + 4);
            vi05 = (std::uint16_t)(vi05 + 2);
        } while (vi08 != vi09);
    }
};

// Whether a batch of vertices of stride quadwords has at least three, whole.
bool Clippable(Runner& r, std::uint16_t top, int stride)
{
    V hdr = Load(r, top + 11);
    std::uint16_t span = (std::uint16_t)(Half(hdr.y) - Half(hdr.x));
    return span % stride == 0 && span / stride >= 3 && span <= 0x400;
}

void ClipIfMarked(Runner& r, std::uint16_t top, std::uint16_t colour)
{
    if (Half(Load(r, top + 13).z) != 0) {
        Clipper c(r);
        c.Run(colour);
    }
}

// Whether a batch can be done here: whole vertices, and at least three when a triangle is marked
// for the clipper.
bool Plain(Runner& r, std::uint16_t top, int& n)
{
    V hdr = Load(r, top + 11);
    n = Count((std::uint16_t)(top + Half(hdr.x)), (std::uint16_t)(top + Half(hdr.y)), 4);
    return n > 0 && (Half(Load(r, top + 13).z) == 0 || n >= 3);
}

// 0x01E's body from 0x020: the colours (0x4C1), the kick of the packet at 12.y, and when 9.w is
// not 1 the second colours (0x4DD) and the packet at 12.z.
void SecondPass(Runner& r, std::uint16_t top, int n)
{
    Colours4C1(r, top, n);
    KickBatch(r, top, Half(Load(r, top + 12).y));
    ClipIfMarked(r, top, 2);
    if (Half(Load(r, top + 9).w) == 1)
        return;
    Colours4DD(r, top, n);
    KickBatch(r, top, Half(Load(r, top + 12).z));
    ClipIfMarked(r, top, 1);
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
        x.v = Ftoi4(Mul4(Itof4(x.v), factor.v));
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
    ClipIfMarked(r, top, 1);
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

// ERLENG: 1 / the length of xyz, or the largest float for 0.
float Erleng(const V& v)
{
    float x = In(v.x), y = In(v.y), z = In(v.z);
    float sum2 = Add(Add(Mul(x, x), Mul(y, y)), Mul(z, z));
    return sum2 == 0.0f ? 3.4028235e38f : core::Fix(1.0f / std::sqrt(sum2));
}

void SetXyz(State& st, int reg, const V& x) { std::memcpy(st.vf[reg], &x, 12); }

bool PointFourWide(Runner& r);

bool PointLight(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    if (Half(Load(r, top + 9).y) == 4)
        return PointFourWide(r);
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

// --- 0x242 of chunk B90790F3: a lit batch's colours made final.
//
// From TOP: 11.x and 11.y the vertices, 12.w the lit colours (floats, one quadword a vertex),
// 13.w whether the batch was set up (0 the first time: each vertex's quadword 2 set from 3 of
// data memory with 13.y / 32768 added to y, the colours cleared, 13.w set, then the rest). Each
// vertex: m the brightest of its lit colour; t = min(quadword 2's xy, (vf01.x - m - vf01.x *
// quadword 2's y, vf01.y - m)), at least 0; the average of its base colour (quadword 1, integers)
// a; quadword 1 becomes a * t.x, quadword 2 the base colour and 255 times t.y, the colour the
// lit colour (at most vf01) times a and 255 times t.x, as integers. The loop is pipelined one
// vertex deep (a vertex is stored in the next one's pass; the first pass stores back what its
// prologue read before the buffers), so it is followed here on the registers themselves.
struct Regs {
    float vf[32][4];
    V get(int r) const { V x = {}; std::memcpy(&x, vf[r], 16); return x; }
    void put(int r, const V& x, int mask) { for (int k = 0; k < 4; k++) if (mask & (8 >> k)) vf[r][k] = x[k]; }
};

bool LitColours(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V hdr = Load(r, top + 11);
    std::uint16_t first = (std::uint16_t)(top + Half(hdr.x));
    int n = Count(first, (std::uint16_t)(top + Half(hdr.y)), 4);
    if (n == 0)
        return false;
    std::uint16_t colours = (std::uint16_t)(top + Half(Load(r, top + 12).w));
    Regs R;
    std::memcpy(R.vf, st.vf, sizeof R.vf);
    const int X = 8, Y = 4, Z = 2, W = 1, XY = 12, XYZ = 14, XYZW = 15;
    V zero = {};
    zero.v = Fix4(Fix4(Splat(0.0f)) - Fix4(F4{0.0f, 0.0f, 0.0f, 1.0f}));   // SUB vf00 - vf00
    zero.w = Sub(1.0f, 1.0f);
    zero.x = Sub(0.0f, 0.0f);
    zero.y = zero.x;
    zero.z = zero.x;
    R.put(15, zero, XYZW);
    auto ftoi = [](V x) { for (int k = 0; k < 4; k++) x[k] = Ftoi0(x[k]); return x; };
    auto itof = [](V x) { for (int k = 0; k < 4; k++) x[k] = Itof0(x[k]); return x; };

    if (Half(Load(r, top + 13).w) == 0) {   // the first time: the setup at 0x26B
        V v16 = R.get(16);
        v16.y = (float)(std::int32_t)Bits(r.Qword(top + 13)[1]) / 32768.0f;   // LQ.y, ITOF15.y
        R.put(16, v16, Y);
        V v09 = R.get(9);
        V g = Load(r, 3);
        v09.x = g.x;
        v09.y = Add(g.y, v16.y);
        R.put(9, v09, XY);
        // 0x24C ran in the branch's delay slot: vf08.w = 255
        V v08 = R.get(8);
        v08.w = Add(zero.w, 255.0f);
        R.put(8, v08, W);
        for (int k = 0; k < n; k++) {
            Store(r, (std::uint16_t)(first + 4 * k + 2), R.get(9));
            Store(r, (std::uint16_t)(colours + k), zero);
        }
        r.Qword(top + 13)[3] = FromBits(1);
    }
    V c255;
    c255.v = Add4(zero.v, Splat(255.0f));
    R.put(3, c255, XYZW);
    V v08 = R.get(8);
    v08.w = Add(zero.w, 255.0f);
    R.put(8, v08, W);
    const float third = FromBits(0x3EAAAA9Fu);
    V c3;
    c3.v = Add4(zero.v, Splat(third));
    R.put(4, c3, XYZW);
    st.i = third;

    // the prologue
    std::uint16_t at = first, col = colours;
    R.put(5, Load(r, at + 1), XYZ);
    R.put(6, Load(r, at + 2), XY);
    R.put(7, Load(r, col), XYZ);
    V t = R.get(8);
    V v05 = R.get(5);
    for (int k = 0; k < 3; k++)
        t[k] = Itof0(v05[k]);
    R.put(8, t, XYZ);
    R.put(11, Load(r, (std::uint16_t)(at - 2)), XYZW);
    R.put(10, Load(r, (std::uint16_t)(at - 3)), XYZ);
    R.put(12, Load(r, (std::uint16_t)(col - 1)), XYZW);
    V v14 = R.get(14);
    v14.x = Max(R.vf[7][0], R.vf[7][1]);
    v14.x = Max(v14.x, R.vf[7][2]);
    R.put(14, v14, X);
    const float* one = st.vf[1];
    for (int k = 0; k < n; k++) {
        // 0x253-0x256: the vertex before stored
        V v02 = R.get(2);
        v02.y = Sub(one[1], R.vf[14][0]);
        float acc = Sub(one[0], R.vf[14][0]);
        v02.x = Sub(acc, Mul(one[0], R.vf[6][1]));
        R.put(2, v02, XY);
        Store(r, (std::uint16_t)(at - 2), R.get(11));
        { V old = Load(r, (std::uint16_t)(at - 3)); V x = R.get(10); old.x = x.x; old.y = x.y; old.z = x.z;
          Store(r, (std::uint16_t)(at - 3), old); }
        Store(r, (std::uint16_t)(col - 1), R.get(12));
        V v12 = R.get(12), v07 = R.get(7);
        for (int c = 0; c < 3; c++)
            v12[c] = Mini(v07[c], one[c]);
        R.put(12, v12, XYZ);
        // 0x257-0x25A: the average of the base colour; the next vertex read
        V v08n = R.get(8);
        F4 a4 = Mul4(c3.v, Splat(v08n.x));
        a4 = Add4(a4, Mul4(c3.v, Splat(v08n.y)));
        R.put(7, Load(r, (std::uint16_t)(col + 1)), XYZ);
        V v09 = R.get(9), v06 = R.get(6);
        v09.x = Mini(v06.x, v02.x);
        v09.y = Mini(v06.y, v02.y);
        R.put(5, Load(r, (std::uint16_t)(at + 5)), XYZ);
        V v13;
        v13.v = Add4(a4, Mul4(c3.v, Splat(v08n.z)));
        R.put(13, v13, XYZ);
        R.put(6, Load(r, (std::uint16_t)(at + 6)), XY);
        // 0x25B-0x265
        V nx = R.get(14);
        nx.x = Max(R.vf[7][0], R.vf[7][1]);
        v09.x = Max(v09.x, 0.0f);
        v09.y = Max(v09.y, 0.0f);
        R.put(9, v09, XY);
        v12 = R.get(12);
        V prod;
        prod.v = Mul4(v12.v, R.get(13).v);
        v12.x = prod.x;
        v12.y = prod.y;
        v12.z = prod.z;
        nx.x = Max(nx.x, R.vf[7][2]);
        R.put(14, nx, X);
        V v11;
        v11.v = Mul4(R.get(8).v, Splat(v09.y));
        V v10;
        v10.v = Mul4(R.get(13).v, Splat(v09.x));
        v12.w = Mul(R.vf[3][3], v09.x);
        V n08 = R.get(8);
        V n05 = R.get(5);
        for (int c = 0; c < 3; c++)
            n08[c] = Itof0(n05[c]);
        R.put(8, n08, XYZ);
        at = (std::uint16_t)(at + 4);
        col = (std::uint16_t)(col + 1);
        R.put(11, ftoi(v11), XYZW);
        R.put(10, ftoi(v10), XYZ);
        R.put(12, ftoi(v12), XYZW);
    }
    // 0x266-0x268: the last vertex stored
    Store(r, (std::uint16_t)(at - 2), R.get(11));
    { V old = Load(r, (std::uint16_t)(at - 3)); V x = R.get(10); old.x = x.x; old.y = x.y; old.z = x.z;
      Store(r, (std::uint16_t)(at - 3), old); }
    Store(r, (std::uint16_t)(col - 1), R.get(12));
    (void)itof;
    std::memcpy(st.vf, R.vf, sizeof R.vf);
    st.pc = 0x26B;
    return true;
}

// --- 0x17E of chunk 6744EAB9: a directional light on a batch's vertices.
//
// From TOP: 4-6 the matrix turning the light's direction (25) into the batch's space, 22, 23 and
// 24 as for the point light. The direction is turned and made unit length once; each vertex: the
// cosine with its normal (quadword 0) as d + d^3, the light (ambient + scale times that, clamped
// to 0..1) added to quadword 2's xy and, through 22 by its z, to the colour. 9.y = 4 (0x0FE) is
// left to the microcode.
bool DirectionalFourWide(Runner& r);

bool DirectionalLight(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    if (Half(Load(r, top + 9).y) == 4)
        return DirectionalFourWide(r);
    V hdr = Load(r, top + 11);
    std::uint16_t first = (std::uint16_t)(top + Half(hdr.x));
    int n = Count(first, (std::uint16_t)(top + Half(hdr.y)), 4);
    if (n == 0)
        return false;
    std::uint16_t colours = (std::uint16_t)(top + Half(Load(r, top + 12).w));
    V dir = Load(r, top + 25);
    V m4 = Load(r, top + 4), m5 = Load(r, top + 5), m6 = Load(r, top + 6);
    F4 acc = Mul4(m4.v, Splat(dir.x));
    acc = Add4(acc, Mul4(m5.v, Splat(dir.y)));
    V turned;
    turned.v = Add4(acc, Mul4(m6.v, Splat(dir.z)));
    float p = Erleng(turned);
    V unit;
    unit.v = Mul4(turned.v, Splat(p));
    float dy = Add(0.0f, unit.y), dz = Add(0.0f, unit.z);
    V colour = Load(r, top + 22), ambient = Load(r, top + 24), scale = Load(r, top + 23);
    V nrm, q2, col, lit;
    float cosine = 0, cube = 0;
    for (int k = 0; k < n; k++) {
        std::uint16_t at = (std::uint16_t)(first + 4 * k);
        nrm = Load(r, at);
        q2 = Load(r, at + 2);
        col = Load(r, (std::uint16_t)(colours + k));
        float a = Mul(unit.x, nrm.x);
        a = Add(a, Mul(dy, nrm.y));
        cosine = Add(a, Mul(dz, nrm.z));
        cube = Mul(cosine, cosine);
        a = Add(cosine, 0.0f);
        cosine = Add(a, Mul(cube, cosine));
        F4 l = Add4(Splat(0.0f), ambient.v);
        lit.v = Add4(l, Mul4(scale.v, Splat(cosine)));
        for (int c = 0; c < 3; c++)
            lit[c] = Mini(Max(lit[c], 0.0f), 1.0f);
        q2.x = Add(q2.x, lit.x);
        q2.y = Add(q2.y, lit.y);
        l = Add4(Splat(0.0f), col.v);
        col.v = Add4(l, Mul4(colour.v, Splat(lit.z)));
        float* out = r.Qword(at + 2);
        out[0] = q2.x;
        out[1] = q2.y;
        std::memcpy(r.Qword((std::uint16_t)(colours + k)), &col, 12);
    }
    SetXyz(st, 2, unit);
    st.vf[3][0] = dy;
    st.vf[4][0] = dz;
    SetXyz(st, 5, ambient);
    SetXyz(st, 6, scale);
    SetXyz(st, 7, colour);
    SetVf(st, 8, nrm);
    st.vf[9][0] = q2.x;
    st.vf[9][1] = q2.y;
    SetXyz(st, 10, col);
    st.vf[11][0] = cosine;
    st.vf[12][0] = cube;
    SetXyz(st, 13, lit);
    SetVf(st, 15, m4);
    SetVf(st, 16, m5);
    SetVf(st, 17, m6);
    st.vf[18][0] = p;
    st.p = p;
    st.pc = 0x1A6;
    return true;
}

// --- 0x401 of chunk 2DDC9AA2 (through 0x3D4): a batch skinned by four matrices.
//
// From TOP: 11.x and 11.y the vertices (stride 13.x, the position in each one's last quadword),
// 11.z the four matrices (16 quadwords), 11.w the weights (one quadword a vertex). Each position:
// through each matrix (rows x, y, z and the translation), the four results blended by the weights.
// The loop at 0x3D4 (and its copy at 0x226): positions at first, every step quadwords, until end,
// through the four matrices at mats and blended by the weights at weights; vf02-vf23 as it
// leaves them. False when the loop would run on past its end.
bool SkinLoop(Runner& r, float (*vf)[4], std::uint16_t first, std::uint16_t end, std::uint16_t step,
              std::uint16_t mats, std::uint16_t weights)
{
    int n = Count(first, end, step);
    if (n == 0 || step > 64)
        return false;
    V m[16];
    for (int k = 0; k < 16; k++)
        m[k] = Load(r, (std::uint16_t)(mats + k));
    V pos, w, t[4];
    std::memcpy(&pos, vf[2], 16);
    std::memcpy(&w, vf[3], 16);
    for (int j = 0; j < 4; j++)
        std::memcpy(&t[j], vf[20 + j], 16);
    // The operands fixed once (the matrices for the batch, a vertex's position and weights for it):
    // Mul4 and Add4 fix only an operand with the largest exponent, which no result has, so on fixed
    // operands they are the host's arithmetic as it is.
    F4 mf[16];
    for (int k = 0; k < 16; k++)
        mf[k] = FixIn4(m[k].v);
    for (int k = 0; k < n; k++) {
        std::uint16_t at = (std::uint16_t)(first + step * k);
        V p = Load(r, at);
        pos.x = p.x;
        pos.y = p.y;
        pos.z = p.z;
        w = Load(r, (std::uint16_t)(weights + k));
        V pf, wf;
        pf.v = FixIn4(p.v);
        wf.v = FixIn4(w.v);
        for (int j = 0; j < 4; j++) {
            F4 acc = mf[4 * j] * Splat(pf.x);
            acc = acc + mf[4 * j + 1] * Splat(pf.y);
            acc = acc + mf[4 * j + 2] * Splat(pf.z);
            V x;
            x.v = acc + mf[4 * j + 3] * Splat(1.0f);
            t[j].x = x.x;
            t[j].y = x.y;
            t[j].z = x.z;
        }
        F4 acc = t[0].v * Splat(wf.x);
        acc = acc + t[1].v * Splat(wf.y);
        acc = acc + t[2].v * Splat(wf.z);
        V out;
        out.v = acc + t[3].v * Splat(wf.w);
        pos.x = out.x;
        pos.y = out.y;
        pos.z = out.z;
        std::memcpy(r.Qword(at), &out, 12);
    }
    std::memcpy(vf[2], &pos, 16);
    std::memcpy(vf[3], &w, 16);
    for (int k = 0; k < 16; k++)
        std::memcpy(vf[4 + k], &m[k], 16);
    for (int j = 0; j < 4; j++)
        std::memcpy(vf[20 + j], &t[j], 16);
    return true;
}

bool Skin(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V hdr = Load(r, top + 11);
    std::uint16_t step = Half(Load(r, top + 13).x);
    std::uint16_t back = (std::uint16_t)(step - 1);
    float vf[32][4];
    std::memcpy(vf, st.vf, sizeof vf);
    if (!SkinLoop(r, vf, (std::uint16_t)(top + Half(hdr.x) + back), (std::uint16_t)(top + Half(hdr.y) + back), step,
                  (std::uint16_t)(top + Half(hdr.z)), (std::uint16_t)(top + Half(hdr.w))))
        return false;
    std::memcpy(st.vf, vf, sizeof vf);
    st.pc = 0x411;
    return true;
}

// --- 0x0FE of chunk E1624411's set (reached from 0x17E and, through 0x142, from 0x1A6 when 9.y
// is 4): a light four vertices at a time.
//
// The batch's normals come four at a time in a group of four vertices, x, y and z in quadword 0
// of its first three. The light's direction (25) is turned by 4-6 and made unit length; each
// group: the four cosines as d + d^3, the light (24 + 23 times that, at least 0) added to each
// vertex's quadword 2 xy and through 22 by its z to its colour. The first time (13.w = 0) quadword
// 2 and the colours start from 3 of data memory (y plus 13.y / 32768) and zero instead of what is
// there; 13.w is then set. 0x142, for a point light, first makes 25 the unit direction from the
// object's centre (10) to the light (26) and scales 23 and 24 by the attenuation there.
// RegsAt reads and writes the registers' fields as the VU's partial loads and stores do.
bool LightFourWide(Runner& r, Regs& R)
{
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V hdr = Load(r, top + 11);
    std::uint16_t first = (std::uint16_t)(top + Half(hdr.x)), end = (std::uint16_t)(top + Half(hdr.y));
    std::uint16_t span = (std::uint16_t)(end - first);
    if (span == 0 || span > 0x400)
        return false;
    const int groups = (span + 15) / 16;
    const int XY = 12, XYZ = 14, XYZW = 15, Y = 4, X = 8;
    V zero;
    zero.v = F4{Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(1.0f, 1.0f)};
    R.put(17, zero, XYZW);
    R.put(17, Load(r, 3), XY);
    for (int k = 21; k <= 24; k++)
        R.put(k, zero, XYZW);
    R.put(31, Load(r, top + 13), Y);
    R.put(2, Load(r, top + 25), XYZ);
    R.put(6, Load(r, top + 4), XYZW);
    R.put(7, Load(r, top + 5), XYZW);
    R.put(8, Load(r, top + 6), XYZW);
    { V v31 = R.get(31); v31.y = (float)(std::int32_t)Bits(v31.y) / 32768.0f; R.put(31, v31, Y); }
    { V v17 = R.get(17); v17.y = Add(v17.y, R.vf[31][1]); R.put(17, v17, Y); }
    V dir = R.get(2);
    F4 acc = Mul4(R.get(6).v, Splat(dir.x));
    acc = Add4(acc, Mul4(R.get(7).v, Splat(dir.y)));
    V turned;
    turned.v = Add4(acc, Mul4(R.get(8).v, Splat(dir.z)));
    R.put(2, turned, XYZ);
    R.put(3, Load(r, top + 22), XYZ);
    R.put(4, Load(r, top + 23), XYZ);
    R.put(5, Load(r, top + 24), XYZ);
    float p = Erleng(R.get(2));
    for (int k = 18; k <= 20; k++) {
        V x;
        x.v = Add4(R.get(21).v, R.get(17).v);
        R.put(k, x, XYZW);
    }
    { V v31 = R.get(31); v31.x = p; R.put(31, v31, X); }
    { V u; u.v = Mul4(R.get(2).v, Splat(p)); R.put(2, u, XYZ); }
    const bool setUp = Half(Load(r, top + 13).w) != 0;
    std::uint16_t colours = (std::uint16_t)(top + Half(Load(r, top + 12).w));
    const V d = R.get(2);
    for (int g = 0; g < groups; g++) {
        std::uint16_t at = (std::uint16_t)(first + 16 * g);
        R.put(25, Load(r, at), XYZW);
        R.put(26, Load(r, (std::uint16_t)(at + 4)), XYZW);
        R.put(27, Load(r, (std::uint16_t)(at + 8)), XYZW);
        F4 a = Mul4(R.get(25).v, Splat(d.x));
        a = Add4(a, Mul4(R.get(26).v, Splat(d.y)));
        V v28;
        v28.v = Add4(a, Mul4(R.get(27).v, Splat(d.z)));
        R.put(28, v28, XYZW);
        V v29;
        v29.v = Mul4(v28.v, v28.v);
        R.put(29, v29, XYZW);
        F4 a2 = Add4(v28.v, Splat(0.0f));
        V v30;
        v30.v = Add4(a2, Mul4(v29.v, v28.v));
        R.put(30, v30, XYZW);
        F4 base = Add4(R.get(5).v, Splat(0.0f));
        if (setUp) {
            for (int j = 0; j < 4; j++)
                R.put(17 + j, Load(r, (std::uint16_t)(at + 2 + 4 * j)), XYZW);
            for (int j = 0; j < 4; j++)
                R.put(21 + j, Load(r, (std::uint16_t)(colours + j)), XYZW);
        }
        const V scale = R.get(4);
        for (int j = 0; j < 4; j++) {
            V l;
            l.v = Add4(base, Mul4(scale.v, Splat(v30[j])));
            for (int c = 0; c < 3; c++)
                l[c] = Max(l[c], 0.0f);
            R.put(9 + j, l, XYZ);
        }
        for (int j = 0; j < 4; j++) {
            V l = R.get(9 + j), q = R.get(17 + j);
            l.x = Add(q.x, l.x);
            l.y = Add(q.y, l.y);
            R.put(9 + j, l, XY);
        }
        const V c22 = R.get(3);
        for (int j = 0; j < 4; j++) {
            F4 ca = Add4(Splat(0.0f), R.get(21 + j).v);
            V c;
            c.v = Add4(ca, Mul4(c22.v, Splat(R.vf[9 + j][2])));
            R.put(13 + j, c, XYZ);
            V l = R.get(9 + j);
            float* q = r.Qword((std::uint16_t)(at + 2 + 4 * j));
            q[0] = l.x;
            q[1] = l.y;
        }
        for (int j = 0; j < 4; j++)
            std::memcpy(r.Qword((std::uint16_t)(colours + j)), R.vf[13 + j], 12);
        colours = (std::uint16_t)(colours + 4);
    }
    r.Qword(top + 13)[3] = FromBits(1);
    r.s.p = p;
    r.s.pc = 0x142;
    return true;
}

bool DirectionalFourWide(Runner& r)
{
    Regs R;
    std::memcpy(R.vf, r.s.vf, sizeof R.vf);
    if (!LightFourWide(r, R))
        return false;
    std::memcpy(r.s.vf, R.vf, sizeof R.vf);
    return true;
}

// 0x142 then 0x0FE.
bool PointFourWide(Runner& r)
{
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V hdr = Load(r, top + 11);
    std::uint16_t span = (std::uint16_t)(Half(hdr.y) - Half(hdr.x));
    if (span == 0 || span > 0x400)
        return false;
    Regs R;
    std::memcpy(R.vf, r.s.vf, sizeof R.vf);
    const int X = 8, XY = 12, XYZ = 14;
    R.put(2, Load(r, top + 10), XYZ);
    R.put(3, Load(r, top + 26), XYZ);
    V v04;
    v04.v = Fix4(Fix4(R.get(3).v) - Fix4(R.get(2).v));
    R.put(4, v04, XYZ);
    R.put(8, Load(r, top + 25), XY);
    float p = Erleng(R.get(4));
    R.put(9, Load(r, top + 23), XYZ);
    R.put(10, Load(r, top + 24), XYZ);
    { V v05 = R.get(5); v05.x = p; R.put(5, v05, X); }
    float a = Add(0.0f, R.vf[8][0]);
    float fade = Add(a, Mul(p, R.vf[8][1]));
    fade = Mini(Max(fade, 0.0f), 1.0f);
    { V v07 = R.get(7); v07.x = fade; R.put(7, v07, X); }
    V u;
    u.v = Mul4(R.get(4).v, Splat(p));
    R.put(4, u, XYZ);
    V c9, c10;
    c9.v = Mul4(R.get(9).v, Splat(fade));
    c10.v = Mul4(R.get(10).v, Splat(fade));
    R.put(9, c9, XYZ);
    R.put(10, c10, XYZ);
    std::memcpy(r.Qword(top + 25), R.vf[4], 12);
    std::memcpy(r.Qword(top + 23), R.vf[9], 12);
    std::memcpy(r.Qword(top + 24), R.vf[10], 12);
    if (!LightFourWide(r, R))
        return false;   // not reached: the span was checked first
    std::memcpy(r.s.vf, R.vf, sizeof R.vf);
    return true;
}

// --- 0x26D of chunk 7B92AF0F: the outlines.
//
// From TOP: 22.x and 22.y the points, 22.z and 22.w the edges (four point indices each: the edge
// from y to z, with x and w the far corners of the two faces it joins), 23.x and 23.y the four
// matrices and weights when the points are skinned first (0x226, as 0x3D4), 14-17 the projection,
// 24 a packet kicked first, 27 and 35 two packets the edges take in turn. The points are projected
// in place (a loop pipelined three deep: a point's w is 0, or for every third the w of the quadword
// before the first point, which the loop loads and stores back). An edge is drawn when its two
// faces' normals (cross products) point opposite ways in z and none of its four points is outside
// the guard band: the edge's ends moved out by a perpendicular of length about 1/4096 (its side by
// the first face's z), to the screen, into the packet's two vertex pairs, the packet kicked. The
// flags the tests read are those of the operations exactly four instructions before.
bool Outlines(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V h22 = Load(r, top + 22), h23 = Load(r, top + 23);
    std::uint16_t base = (std::uint16_t)(top + Half(h22.x)), end = (std::uint16_t)(top + Half(h22.y));
    std::uint16_t edge = (std::uint16_t)(top + Half(h22.z)), edgeEnd = (std::uint16_t)(top + Half(h22.w));
    if (edge == edgeEnd || (std::uint16_t)(edgeEnd - edge) > 0x400 || (std::uint16_t)(end - base) > 0x400)
        return false;
    float vf[32][4];
    std::memcpy(vf, st.vf, sizeof vf);
    auto get = [&](int k) { V x = {}; std::memcpy(&x, vf[k], 16); return x; };
    auto put = [&](int k, const V& x, int mask) {
        for (int c = 0; c < 4; c++)
            if (mask & (8 >> c))
                vf[k][c] = x[c];
    };
    const int XY = 12, XYZ = 14, XYZW = 15;
    if (Half(h23.x) != 0 &&
        !SkinLoop(r, vf, base, end, 1, (std::uint16_t)(top + Half(h23.x)), (std::uint16_t)(top + Half(h23.y))))
        return false;

    // the projection, 0x279-0x295
    V zero;
    zero.v = F4{Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(1.0f, 1.0f)};
    V m[4] = {Load(r, top + 14), Load(r, top + 15), Load(r, top + 16), Load(r, top + 17)};
    auto project = [&](const V& p) { return Transform(m, p); };
    V v08, v09, v10, v19, v20 = zero, v21 = zero, v22, v23, v24;
    v09 = Load(r, base);
    v23 = project(v09);
    v10 = Load(r, (std::uint16_t)(base + 1));
    v08 = Load(r, (std::uint16_t)(base + 2));
    v19 = Load(r, (std::uint16_t)(base - 1));
    float qOf23 = Reciprocal(r, v23.w);
    V scale = Load(r, 0), offset = Load(r, 1);
    v24 = project(v10);
    float qOf24 = Reciprocal(r, v24.w), qOf22 = 0;
    std::uint16_t at = (std::uint16_t)(base - 1);
    Store(r, at, v19);
    at = (std::uint16_t)(at + 1);
    auto mulq = [](V& d, const V& a, float q) {
        d.x = Mul(a.x, q);
        d.y = Mul(a.y, q);
        d.z = Mul(a.z, q);
    };
    for (;;) {
        v22 = project(v08);
        v09 = Load(r, (std::uint16_t)(at + 3));
        mulq(v20, v23, qOf23);
        qOf22 = Reciprocal(r, v22.w);   // in the exit's delay slot too
        if (at == end)
            break;
        Store(r, at, v20);
        at = (std::uint16_t)(at + 1);
        v23 = project(v09);
        v10 = Load(r, (std::uint16_t)(at + 3));
        mulq(v21, v24, qOf24);
        qOf23 = Reciprocal(r, v23.w);
        if (at == end)
            break;
        Store(r, at, v21);
        at = (std::uint16_t)(at + 1);
        v24 = project(v10);
        v08 = Load(r, (std::uint16_t)(at + 3));
        mulq(v19, v22, qOf22);
        if (at == end)
            break;
        qOf24 = Reciprocal(r, v24.w);
        Store(r, at, v19);
        at = (std::uint16_t)(at + 1);
    }
    (void)qOf22;
    for (int c = 0; c < 4; c++)
        put(4 + c, m[c], XYZW);
    put(2, scale, XYZW);
    put(3, offset, XYZW);
    put(19, v19, XYZW);
    put(20, v20, XYZW);
    put(21, v21, XYZW);
    put(22, v22, XYZW);
    put(23, v23, XYZW);
    put(24, v24, XYZW);

    // the edges, 0x296-0x2D7
    std::uint16_t packet = 27;
    auto corners = [&](std::uint16_t e, V* out) {
        V idx = Load(r, e);
        for (int k = 0; k < 4; k++)
            out[k] = Load(r, (std::uint16_t)(Half(idx[k]) + base));
    };
    V pts[4];
    corners(edge, pts);
    V v11, v12 = get(12), v13 = get(13), v14 = get(14), v15 = get(15), v16 = get(16), v17 = get(17), v18 = get(18);
    V v25 = get(25), v26 = get(26), v27 = get(27), v28 = get(28), v29 = get(29), v30 = get(30);
    v08 = pts[0];
    v09 = pts[1];
    v10 = pts[2];
    v11 = pts[3];
    Kick(r, (std::uint16_t)(top + 24));
    std::uint32_t clip = st.clip;
    float p = st.p, i = st.i;
    auto code = [&](const V& x) { V c = x; c.w = 1.0f; return ClipCode(c); };
    auto sub3 = [](V& d, const V& a, const V& b) {
        d.x = Sub(a.x, b.x);
        d.y = Sub(a.y, b.y);
        d.z = Sub(a.z, b.z);
    };
    // OPMULA a, b then OPMSUB d, b, a: a x b's fields
    auto cross = [](V& d, const V& a, const V& b) {
        for (int k = 0; k < 3; k++)
            d[k] = Sub(Mul(a[(k + 1) % 3], b[(k + 2) % 3]), Mul(b[(k + 1) % 3], a[(k + 2) % 3]));
    };
    do {
        sub3(v15, v10, v09);
        sub3(v14, v09, v08);
        sub3(v16, v11, v10);
        std::uint32_t c0 = code(v08);
        edge = (std::uint16_t)(edge + 1);
        v12.x = Mul(v15.x, v15.x);
        v12.y = Mul(v15.y, v15.y);
        cross(v18, v14, v15);
        cross(v17, v16, v15);
        std::uint32_t c1 = code(v09);
        v25 = v09;
        std::uint32_t c2 = code(v10);
        v26 = v10;
        std::uint32_t c3 = code(v11);
        clip = (c0 << 18) | (c1 << 12) | (c2 << 6) | c3;
        corners(edge, pts);
        v08 = pts[0];
        v09 = pts[1];
        v10 = pts[2];
        v11 = pts[3];
        float facing = Mul(v18.z, v17.z);
        bool opposite = std::signbit(facing) && facing != 0.0f;
        v12.x = Add(v12.x, v12.y);
        bool outside = clip != 0;
        float side = Add(v18.z, 0.0f);
        bool negative = std::signbit(side) && side != 0.0f;
        {   // ERSQRT P, vf12x, in the branch's delay slot
            float d = std::sqrt(std::fabs(In(v12.x)));
            p = d == 0.0f ? 3.4028235e38f : core::Fix(1.0f / d);
        }
        if (outside)
            continue;
        v29.x = Add(0.0f, v15.y);
        v29.y = Sub(0.0f, v15.x);
        if (!opposite)
            continue;
        i = FromBits(0x397BA882u);
        v30.x = Mul(v15.x, i);
        v30.y = Mul(v15.y, i);
        if (negative)
            i = FromBits(0xB97BA882u);
        v29.x = Mul(v29.x, i);
        v29.y = Mul(v29.y, i);
        v30.x = Add(v30.x, v29.x);
        v30.y = Add(v30.y, v29.y);
        v13.x = p;
        v29.x = Mul(v29.x, p);
        v29.y = Mul(v29.y, p);
        v30.x = Mul(v30.x, p);
        v30.y = Mul(v30.y, p);
        v25.x = Add(v25.x, v29.x);
        v25.y = Add(v25.y, v29.y);
        v26.x = Add(v26.x, v30.x);
        v26.y = Add(v26.y, v30.y);
        v27.v = Add4(Mul4(v25.v, scale.v), Mul4(offset.v, Splat(1.0f)));
        v28.v = Add4(Mul4(v26.v, scale.v), Mul4(offset.v, Splat(1.0f)));
        std::uint16_t q = (std::uint16_t)(packet + top);
        Store(r, (std::uint16_t)(q + 2), v27);
        Store(r, (std::uint16_t)(q + 6), v27);
        Store(r, (std::uint16_t)(q + 3), v28);
        Store(r, (std::uint16_t)(q + 7), v28);
        packet = (std::uint16_t)(62 - packet);
        Kick(r, q);
    } while (edge != edgeEnd);
    put(8, v08, XYZW);
    put(9, v09, XYZW);
    put(10, v10, XYZW);
    put(11, v11, XYZW);
    put(12, v12, XY);
    put(13, v13, XYZW);
    put(14, v14, XYZ);
    put(15, v15, XYZ);
    put(16, v16, XYZ);
    put(17, v17, XYZ);
    put(18, v18, XYZ);
    put(25, v25, XYZW);
    put(26, v26, XYZW);
    put(27, v27, XYZW);
    put(28, v28, XYZW);
    put(29, v29, XY);
    put(30, v30, XY);
    std::memcpy(st.vf, vf, sizeof vf);
    st.clip = clip;
    st.p = p;
    st.i = i;
    st.pc = 0x2DA;
    return true;
}

// --- 0x076 of chunk E1624411: sprites.
//
// From TOP: 22-25 a matrix (multiplied first by the one at 8-11 of data memory, 0x22B, in place),
// 8.xy the w of the viewport's offset and scale, 26 the sprites' colour, 27 the size's limits
// (x, y) and where it fades (w), 11.x and 11.y the sprites (stride 13.x: quadword 0 the centre's
// w, the last the centre), 28 a packet kicked first. Each sprite: its centre less the viewport's
// offset, over its scale, times its w, through the matrix; its size (w) kept within the limits,
// and when kept, the centre moved by half the change and the colour's w 0 past the fade; then
// divided by the sprite's w into quadword 0; the colour into quadwords 1 and stride - 2. The
// batch is kicked; one with a sprite to clip (13.z) is left to the microcode.

// 0x22B: rows 0-3 at out = the rows at in times the matrix at global (each result row k is the
// in rows weighted by global row k), vf20-vf31 as it leaves them.
void MatrixProduct(Runner& r, float (*vf)[4], std::uint16_t in, std::uint16_t global, std::uint16_t out)
{
    V g[4], a[4], res[4];
    for (int k = 0; k < 4; k++) {
        g[k] = Load(r, (std::uint16_t)(global + k));
        a[k] = Load(r, (std::uint16_t)(in + k));
    }
    for (int k = 0; k < 4; k++) {
        F4 acc = Mul4(a[0].v, Splat(g[k].x));
        acc = Add4(acc, Mul4(a[1].v, Splat(g[k].y)));
        acc = Add4(acc, Mul4(a[2].v, Splat(g[k].z)));
        res[k].v = Add4(acc, Mul4(a[3].v, Splat(g[k].w)));
    }
    for (int k = 0; k < 4; k++)
        Store(r, (std::uint16_t)(out + k), res[k]);
    for (int k = 0; k < 4; k++) {
        std::memcpy(vf[20 + k], &a[k], 16);
        std::memcpy(vf[24 + k], &g[k], 16);
        std::memcpy(vf[28 + k], &res[k], 16);
    }
}

bool Clippable(Runner& r, std::uint16_t top, int stride);

bool Sprites(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V hdr = Load(r, top + 11);
    std::uint16_t step = Half(Load(r, top + 13).x);
    const bool clips = Half(Load(r, top + 13).z) != 0;
    // the clipper after: 0x671 for three quadwords, else 0x5EF (which steps by four)
    if (clips && !((step == 3 && Clippable(r, top, 3)) || (step == 4 && Clippable(r, top, 4))))
        return false;
    std::uint16_t first = (std::uint16_t)(top + Half(hdr.x)), end = (std::uint16_t)(top + Half(hdr.y));
    std::uint16_t last = (std::uint16_t)(end - step);
    std::uint16_t span = (std::uint16_t)(last - first);
    if (step == 0 || step > 64 || span % step || span / step > 1024)
        return false;
    const int n = span / step + 1;
    float vf[32][4];
    std::memcpy(vf, st.vf, sizeof vf);
    auto get = [&](int k) { V x = {}; std::memcpy(&x, vf[k], 16); return x; };
    auto set = [&](int k, const V& x) { std::memcpy(vf[k], &x, 16); };
    MatrixProduct(r, vf, (std::uint16_t)(top + 22), 8, (std::uint16_t)(top + 22));
    V m[4] = {Load(r, top + 22), Load(r, top + 23), Load(r, top + 24), Load(r, top + 25)};
    for (int k = 0; k < 4; k++)
        set(2 + k, m[k]);
    V colour = Load(r, top + 26), limits = Load(r, top + 27), v09 = Load(r, top + 8);
    V v06 = Load(r, 1), v07 = Load(r, 0);
    v06.w = Mul(1.0f, v09.x);
    v07.w = Mul(1.0f, v09.y);
    V v08 = get(8);
    v08.x = Add(0.0f, Reciprocal(r, v07.x));
    v08.y = Add(0.0f, Reciprocal(r, v07.y));
    v08.z = Add(0.0f, Reciprocal(r, v07.z));
    float q = 0.0f;
    Kick(r, (std::uint16_t)(top + 28));
    V v10 = get(10), v12 = get(12), v13 = get(13), v14 = get(14), v11 = limits;
    float i = st.i;
    const std::uint16_t back1 = (std::uint16_t)(step - 1), back2 = (std::uint16_t)(step - 2);
    for (int k = 0; k < n; k++) {
        std::uint16_t at = (std::uint16_t)(first + step * k);
        v13.w = Load(r, at).w;
        V c = Load(r, (std::uint16_t)(at + back1));
        v12.x = c.x;
        v12.y = c.y;
        v12.z = c.z;
        V d;
        d.v = Fix4(Fix4(v12.v) - Fix4(v06.v));
        v12.x = d.x;
        v12.y = d.y;
        v12.z = d.z;
        q = Reciprocal(r, v13.w);
        v12.w = v13.w;
        for (int j = 0; j < 3; j++)
            v12[j] = Mul(v12[j], v08[j]);
        for (int j = 0; j < 3; j++)
            v12[j] = Mul(v12[j], v12.w);
        F4 acc = Mul4(m[0].v, Splat(v12.x));
        acc = Add4(acc, Mul4(m[1].v, Splat(v12.y)));
        acc = Add4(acc, Mul4(m[2].v, Splat(v12.z)));
        v13.v = Add4(acc, Mul4(m[3].v, Splat(v12.w)));
        float below = Sub(v13.w, v11.x), above = Sub(v13.w, v11.y);
        v10 = colour;
        v13.z = Add(0.0f, v13.w);
        bool small = std::signbit(below) && below != 0.0f;
        bool withinTop = std::signbit(above) && above != 0.0f;
        v11.z = Add(0.0f, v11.x);
        bool kept = small;
        if (!small) {
            v11.z = Add(0.0f, v11.y);
            kept = !withinTop;
        }
        if (kept) {   // 0x0C5
            v14.z = Sub(v11.z, v13.w);
            i = 0.5f;
            v14.z = Mul(v14.z, i);
            v13.z = v11.z;
            v13.x = Add(v13.x, v14.z);
            v13.y = Add(v13.y, v14.z);
            float fade = Sub(v13.w, v11.w);
            if (std::signbit(fade) && fade != 0.0f)
                v10.w = Sub(1.0f, 1.0f);
        }
        for (int j = 0; j < 3; j++)
            v13[j] = Mul(v13[j], q);
        Store(r, (std::uint16_t)(at + 1), v10);
        Store(r, (std::uint16_t)(at + back2), v10);
        std::memcpy(r.Qword(at), &v13, 12);
    }
    Kick(r, (std::uint16_t)(first - 1));
    set(6, v06);
    set(7, v07);
    set(8, v08);
    set(9, v09);
    set(10, v10);
    set(11, v11);
    set(12, v12);
    set(13, v13);
    set(14, v14);
    set(15, colour);
    std::memcpy(st.vf, vf, sizeof vf);
    st.q = q;
    st.i = i;
    if (clips) {
        Clipper c(r);
        if (step == 3)
            c.Run3();
        else
            c.Run(1);
    }
    st.pc = 0x0C5;
    return true;
}

// --- 0x110 of chunk 1AE4A8D6 (set B): particles, as rotated quads.
//
// From TOP: 0.x and 0.y the particles (ten quadwords each), 0.z the count for the GIF tag before
// them, 1-4 and 6 quadratics in the particle's distance (size, alpha, size again, frame, blend),
// 5 the distance's origin (w) and the quads' xy scale, 7-10 a matrix, 11.x the alpha's scale,
// 15 on the frames' texture registers, 47 on the colours. Each particle: its angle folded into
// [-pi/2, pi/2) by a quarter turn either way (the corners' order turned to match; the tests read
// the MAC flags four instructions on), its sine (ESIN) and cosine (ESQRT of 1 - sin^2); its
// frame (a quadratic in distance, plus 2.y, wrapped by 6.z), its colour (blended along the
// gradient by another when 6.x's bit 0 asks), its alpha (a third, clamped, times 11.x), the size
// (a fourth) rotating and scaling the four corners about the centre (through 7-10 and the matrix
// at 4-7 of data memory); a quad with a corner outside is skipped, its first two vertices marked
// (ADC); else the corners to the screen with fixed STQs. The packet at 48 is kicked.
bool Particles(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V h0 = Load(r, top);
    std::uint16_t vi06 = (std::uint16_t)(Half(h0.x) + top), vi07 = (std::uint16_t)(Half(h0.y) + top);
    std::uint16_t span = (std::uint16_t)(vi07 - vi06);
    if (span == 0 || span % 10 || span > 0x400)
        return false;
    float (*vf)[4] = st.vf;
    auto g = [&](int k) { V x = {}; std::memcpy(&x, vf[k], 16); return x; };
    auto p = [&](int k, const V& x, int mask) {
        for (int c = 0; c < 4; c++)
            if (mask & (8 >> c))
                vf[k][c] = x[c];
    };
    auto lq = [&](int k, std::uint16_t a, int mask) { p(k, Load(r, a), mask); };
    const int X = 8, Y = 4, Z = 2, W = 1, XY = 12, XYZ = 14, XYZW = 15;
    const std::uint16_t vi05 = 32768;
    lq(2, 0, XYZW);
    lq(3, 1, XYZW);
    lq(4, (std::uint16_t)(top + 5), XYZW);
    std::uint16_t vi09 = (std::uint16_t)(Half(Load(r, (std::uint16_t)(top + 6)).z) - 1);
    std::uint16_t vi08 = Half(Load(r, (std::uint16_t)(top + 6)).x);
    for (int k = 11; k <= 14; k++)
        for (int c = 0; c < 3; c++)
            vf[k][c] = Add(0.0f, 0.0f);
    for (int k = 0; k < 4; k++)
        lq(5 + k, (std::uint16_t)(top + 1 + k), XYZW);
    r.Qword((std::uint16_t)(vi06 - 1))[0] = FromBits((std::uint16_t)(Half(h0.z) + vi05));
    vf[9][0] = Add(0.0f, 1.0f);
    vf[10][3] = Add(1.0f, 0.0f);
    for (int k = 0; k < 4; k++)
        lq(16 + k, (std::uint16_t)(4 + k), XYZW);
    const float one = vf[1][0];
    const float halfPi = FromBits(0x3FC90FDBu);
    auto corners = [&](const float (*xy)[2]) {
        for (int k = 0; k < 4; k++) {
            vf[11 + k][0] = xy[k][0];
            vf[11 + k][1] = xy[k][1];
        }
    };
    const float m1 = Sub(0.0f, 1.0f), p1 = Add(0.0f, 1.0f);
    auto quadratic = [&](int coeff) {   // MUL.xyz vf21 = coeff * vf09, ADDAy.x, MADDz.x with vf01.x
        V c = g(coeff), d = g(9);
        V t = g(21);
        for (int k = 0; k < 3; k++)
            t[k] = Mul(c[k], d[k]);
        p(21, t, XYZ);
        float acc = Add(vf[21][0], vf[21][1]);
        vf[21][0] = Add(acc, Mul(one, vf[21][2]));
    };
    auto clamp01 = [&]() { vf[21][0] = Max(Mini(vf[21][0], 1.0f), 0.0f); };
    do {
        lq(21, (std::uint16_t)(vi06 + 2), W);
        float a = Add(0.0f, vf[21][3]);
        st.i = halfPi;
        if (std::signbit(a) && a != 0.0f) {
            vf[22][3] = Add(vf[21][3], st.i);
            if (std::signbit(vf[22][3]) && vf[22][3] != 0.0f) {
                const float c[4][2] = {{m1, p1}, {m1, m1}, {p1, p1}, {p1, m1}};
                corners(c);
                vf[21][3] = vf[22][3];
            } else {
                const float c[4][2] = {{m1, m1}, {p1, m1}, {m1, p1}, {p1, p1}};
                corners(c);
            }
        } else {
            vf[22][3] = Sub(vf[21][3], st.i);
            if (!(std::signbit(vf[22][3]) && vf[22][3] != 0.0f)) {
                const float c[4][2] = {{p1, m1}, {p1, p1}, {m1, m1}, {m1, p1}};
                corners(c);
                vf[21][3] = vf[22][3];
            } else {
                const float c[4][2] = {{m1, m1}, {p1, m1}, {m1, p1}, {p1, p1}};
                corners(c);
            }
        }
        float sine = core::Fix(std::sin(In(vf[21][3])));   // ESIN
        lq(20, (std::uint16_t)(vi06 + 3), XYZW);
        std::uint16_t vi11 = Half(Load(r, (std::uint16_t)(vi06 + 2)).x);
        vf[9][3] = Sub(vf[4][3], vf[20][3]);
        vf[9][1] = Add(0.0f, vf[9][3]);
        vf[9][2] = Add(0.0f, vf[9][3]);
        vf[9][2] = Mul(vf[9][2], vf[9][2]);
        quadratic(7);
        vf[21][0] = Ftoi0(vf[21][0]);
        std::uint16_t vi01 = Half(Load(r, (std::uint16_t)(vi06 + 2)).y);
        std::uint16_t vi10 = (std::uint16_t)((vi01 + Half(vf[21][0])) & vi09);
        vi01 = (std::uint16_t)(top - vi11);
        lq(24, (std::uint16_t)(vi01 + 47), XYZW);
        if (vi08 & 1) {
            V c24 = g(24);
            for (int k = 0; k < 4; k++)
                c24[k] = Itof0(c24[k]);
            p(24, c24, XYZW);
            quadratic(8);
            clamp01();
            vf[22][0] = Sub(one, vf[21][0]);
            vi01 = (std::uint16_t)(vi01 - 1);
            F4 acc = Mul4(g(24).v, Splat(vf[22][0]));
            lq(24, (std::uint16_t)(vi01 + 47), XYZW);
            V n = g(24);
            for (int k = 0; k < 4; k++)
                n[k] = Itof0(n[k]);
            n.v = Add4(acc, Mul4(n.v, Splat(vf[21][0])));
            for (int k = 0; k < 4; k++)
                n[k] = Ftoi0(n[k]);
            p(24, n, XYZW);
        }
        quadratic(5);
        vi10 = (std::uint16_t)(vi10 + top);
        lq(23, (std::uint16_t)(vi10 + 15), XYZW);
        Store(r, vi06, g(23));
        clamp01();
        vf[24][3] = Itof0(vf[24][3]);
        lq(22, (std::uint16_t)(top + 11), X);
        vf[24][3] = Mul(vf[24][3], vf[21][0]);
        vf[24][3] = Mul(vf[24][3], vf[22][0]);
        vf[24][3] = Ftoi0(vf[24][3]);
        p(10, g(20), XYZ);
        {
            V v10 = g(10);
            F4 acc = Mul4(Load(r, (std::uint16_t)(top + 7)).v, Splat(v10.x));
            acc = Add4(acc, Mul4(Load(r, (std::uint16_t)(top + 8)).v, Splat(v10.y)));
            acc = Add4(acc, Mul4(Load(r, (std::uint16_t)(top + 9)).v, Splat(v10.z)));
            V last = Load(r, (std::uint16_t)(top + 10));
            p(22, last, XYZW);
            V o;
            o.v = Add4(acc, Mul4(last.v, Splat(v10.w)));
            p(10, o, XYZW);
        }
        quadratic(6);
        Store(r, (std::uint16_t)(vi06 + 2), g(24));
        for (int k = 11; k <= 14; k++) {
            vf[k][0] = Mul(vf[k][0], vf[21][0]);
            vf[k][1] = Mul(vf[k][1], vf[21][0]);
        }
        vf[21][1] = sine;
        vf[22][1] = vf[21][1];
        vf[22][1] = Mul(vf[22][1], vf[22][1]);
        vf[22][1] = Sub(vf[1][1], vf[22][1]);
        vf[22][0] = Sub(0.0f, vf[21][1]);
        float cosine = core::Fix(std::sqrt(std::fabs(In(vf[22][1]))));   // ESQRT
        {
            V v10 = g(10);
            F4 acc = Mul4(g(16).v, Splat(v10.x));
            acc = Add4(acc, Mul4(g(17).v, Splat(v10.y)));
            acc = Add4(acc, Mul4(g(18).v, Splat(v10.z)));
            V o;
            o.v = Add4(acc, Mul4(g(19).v, Splat(v10.w)));
            p(25, o, XYZW);
        }
        vf[22][1] = cosine;
        st.p = cosine;
        vf[21][0] = Add(0.0f, vf[22][1]);
        for (int k = 11; k <= 14; k++) {
            float x = vf[k][0], y = vf[k][1];
            vf[31][0] = Add(Mul(vf[21][0], x), Mul(vf[22][0], y));
            vf[31][1] = Add(Mul(vf[21][1], x), Mul(vf[22][1], y));
            vf[k][0] = Mul(vf[31][0], vf[4][0]);
            vf[k][1] = Mul(vf[31][1], vf[4][1]);
        }
        float q = Reciprocal(r, vf[25][3]);
        st.q = q;
        std::uint32_t clip = st.clip;
        for (int k = 0; k < 4; k++) {
            V c = g(27 + k);
            for (int j = 0; j < 3; j++)
                c[j] = Add(vf[25][j], vf[11 + k][j]);
            p(27 + k, c, XYZ);
        }
        for (int k = 0; k < 4; k++) {
            V c = g(27 + k);
            c.w = vf[25][3];
            clip = ((clip << 6) | ClipCode(c)) & 0xFFFFFF;
        }
        st.clip = clip;
        if (clip) {
            for (int k = 0; k < 4; k++)
                r.Qword((std::uint16_t)(vi06 + 3 + 2 * k))[3] = FromBits(vi05);
        } else {
            for (int j = 0; j < 3; j++)
                vf[26][j] = Mul(vf[2][j], q);
            const std::uint32_t wbits[4] = {0xFFFF8000u, 0xFFFF8000u, 0u, 0u};
            for (int k = 0; k < 4; k++) {
                for (int j = 0; j < 3; j++)
                    vf[15][j] = Add(Mul(vf[26][j], vf[27 + k][j]), Mul(vf[1][j], vf[3][j]));
                vf[15][3] = FromBits(wbits[k]);
                Store(r, (std::uint16_t)(vi06 + 3 + 2 * k), g(15));
            }
            const float zero = Add(0.0f, 0.0f), unit = Add(0.0f, 1.0f);
            vf[21][0] = zero;
            vf[21][1] = unit;
            vf[21][2] = unit;
            Store(r, (std::uint16_t)(vi06 + 1), g(21));
            vf[21][0] = unit;
            vf[21][1] = unit;
            vf[21][2] = unit;
            Store(r, (std::uint16_t)(vi06 + 4), g(21));
            vf[21][0] = zero;
            vf[21][1] = zero;
            vf[21][2] = unit;
            Store(r, (std::uint16_t)(vi06 + 6), g(21));
            vf[21][1] = zero;
            vf[21][0] = unit;
            vf[21][2] = unit;
            Store(r, (std::uint16_t)(vi06 + 8), g(21));
        }
        vi06 = (std::uint16_t)(vi06 + 10);
    } while (vi06 != vi07);
    Kick(r, (std::uint16_t)(top + 48));
    st.pc = 0x1DB;
    (void)Y;
    (void)Z;
    (void)XY;
    return true;
}

// --- 0x411 of chunk 2DDC9AA2: a batch's normals skinned and laid out four at a time.
//
// From TOP: 11.x and 11.y the vertices (four quadwords, the normal in quadword 0), 11.z the four
// matrices, 11.w the weights. Each normal goes through the four matrices' 3x3 parts, blended by
// its weights, is made unit length (ERLENG) and joins vf25, vf26 and vf27 (x, y and z of four
// normals, the newest in w, MR32 moving the others along); every four, they and vf28 (zero) are
// stored over quadword 0 of the group's four vertices, for 0x0FE. The loop is pipelined two
// deep: it runs two passes past the end, and P, read by MFP ten instructions after the ERLENG
// before it, holds the ERLENG of two passes back (the first two passes read the P, and divide the
// vf29 and vf30, a program before left). A last group short of four is rotated into place and
// stored as it is.
bool SkinNormals(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V hdr = Load(r, top + 11);
    std::uint16_t vi05 = (std::uint16_t)(top + Half(hdr.x)), vi06 = (std::uint16_t)(top + Half(hdr.y) + 8);
    std::uint16_t span = (std::uint16_t)(vi06 - vi05);
    if (span == 0 || (span & 3) || span > 0x400)
        return false;
    float (*vf)[4] = st.vf;
    auto g = [&](int k) { V x = {}; std::memcpy(&x, vf[k], 16); return x; };
    auto p = [&](int k, const V& x, int mask) {
        for (int c = 0; c < 4; c++)
            if (mask & (8 >> c))
                vf[k][c] = x[c];
    };
    const int XYZ = 14, XYZW = 15;
    std::uint16_t mats = (std::uint16_t)(top + Half(hdr.z)), vi08 = (std::uint16_t)(top + Half(hdr.w));
    for (int k = 0; k < 16; k++)
        p(4 + k, Load(r, (std::uint16_t)(mats + k)), XYZW);
    p(2, Load(r, vi05), XYZ);
    p(3, Load(r, vi08), XYZW);
    vi08 = (std::uint16_t)(vi08 + 1);
    V zero;
    zero.v = F4{Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(1.0f, 1.0f)};
    p(28, zero, XYZW);
    int vi09 = 6;
    float visible = st.p;   // what MFP reads
    float pending = 0.0f;   // the ERLENG issued in the pass before, not yet done
    bool havePending = false;
    auto mr32 = [&](int k) {
        V x = g(k), y;
        for (int c = 0; c < 4; c++)
            y[c] = x[(c + 1) & 3];
        p(k, y, XYZW);
    };
    auto turn = [&](int row, const V& n) {
        F4 acc = Mul4(g(row).v, Splat(n.x));
        acc = Add4(acc, Mul4(g(row + 1).v, Splat(n.y)));
        V o;
        o.v = Add4(acc, Mul4(g(row + 2).v, Splat(n.z)));
        return o;
    };
    do {
        mr32(25);
        mr32(26);
        mr32(27);
        V n = g(2);
        p(20, turn(4, n), XYZ);
        p(21, turn(8, n), XYZ);
        p(22, turn(12, n), XYZ);
        vf[24][0] = visible;
        p(23, turn(16, n), XYZ);
        {
            V u = g(30);
            V o;
            o.v = Mul4(u.v, Splat(vf[24][0]));
            p(2, o, XYZ);
        }
        vi09--;
        V w = g(3);
        F4 acc = Mul4(g(20).v, Splat(w.x));
        acc = Add4(acc, Mul4(g(21).v, Splat(w.y)));
        acc = Add4(acc, Mul4(g(22).v, Splat(w.z)));
        V blend;
        blend.v = Add4(acc, Mul4(g(23).v, Splat(w.w)));
        p(30, g(29), XYZW);
        p(29, blend, XYZ);
        vf[25][3] = Add(vf[28][3], vf[2][0]);
        vf[26][3] = Add(vf[28][3], vf[2][1]);
        vf[27][3] = Add(vf[28][3], vf[2][2]);
        vi05 = (std::uint16_t)(vi05 + 4);
        if (vi09 == 0) {
            Store(r, (std::uint16_t)(vi05 - 12), g(28));
            Store(r, (std::uint16_t)(vi05 - 24), g(25));
            Store(r, (std::uint16_t)(vi05 - 20), g(26));
            Store(r, (std::uint16_t)(vi05 - 16), g(27));
            vi09 = 4;
        }
        p(2, Load(r, vi05), XYZ);
        p(3, Load(r, vi08), XYZW);
        vi08 = (std::uint16_t)(vi08 + 1);
        // the ERLENG of the pass before is done by the next MFP; this one is not
        if (havePending)
            visible = pending;
        pending = Erleng(g(29));
        havePending = true;
    } while (vi05 != vi06);
    st.p = pending;
    if (vi09 - 4 < 0) {
        do {
            mr32(25);
            mr32(26);
            mr32(27);
            vi09--;
            vi05 = (std::uint16_t)(vi05 + 4);
        } while (vi09 != 0);
        Store(r, (std::uint16_t)(vi05 - 24), g(25));
        Store(r, (std::uint16_t)(vi05 - 20), g(26));
        Store(r, (std::uint16_t)(vi05 - 16), g(27));
        Store(r, (std::uint16_t)(vi05 - 12), g(28));
    }
    st.pc = 0x45C;
    return true;
}

// --- 0x318 of chunk 79A6F5FB: the matrix at 14-17 made from 4-7 of data memory and 0-3 (0x22B).
bool MatrixAt14(Runner& r)
{
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    MatrixProduct(r, r.s.vf, 4, top, (std::uint16_t)(top + 14));
    r.s.pc = 0x31F;
    return true;
}

// --- 0x31F of chunk 79A6F5FB: quadwords 0-10 and 14-21 copied from the other buffer (572 - TOP).
bool CopyFromOtherBuffer(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top, from = (std::uint16_t)(572 - top);
    V v[11];
    for (int k = 0; k < 11; k++)
        v[k] = Load(r, (std::uint16_t)(from + k));
    for (int k = 0; k < 11; k++)
        Store(r, (std::uint16_t)(top + k), v[k]);
    for (int k = 0; k < 11; k++)
        SetVf(st, 2 + k, v[k]);
    for (int half = 0; half < 2; half++) {
        V w[4];
        for (int k = 0; k < 4; k++)
            w[k] = Load(r, (std::uint16_t)(from + 14 + 4 * half + k));
        for (int k = 0; k < 4; k++)
            Store(r, (std::uint16_t)(top + 14 + 4 * half + k), w[k]);
        for (int k = 0; k < 4; k++)
            SetVf(st, 2 + k, w[k]);
    }
    st.pc = 0x34A;
    return true;
}

// --- 0x05F of chunk E1624411: an unlit batch's finish (0x4A9's scaling, the kick of the packet at
// 12.x and the batch); one with a triangle marked for the clipper (0x671) is left to the microcode.
bool UnlitFinish(Runner& r)
{
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    const bool clips = Half(Load(r, top + 13).z) != 0;
    if (clips && !Clippable(r, top, 3))
        return false;
    State saved = r.s;
    if (!Scale4A9(r, top)) {
        r.s = saved;
        return false;
    }
    KickBatch(r, top, Half(Load(r, top + 12).x));
    if (clips) {
        Clipper c(r);
        c.Run3();
    }
    r.s.pc = 0x076;
    return true;
}

// --- 0x051 of chunk E1624411: a batch's setup. 13.w is set (in a branch's delay slot) and, by
// 9.y: 1, the colours converted (0x0E1); 2 or 3, quadword 2 and the colours started (0x0D2);
// otherwise 13.w cleared again.
bool BatchSetup(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    std::uint16_t kind = Half(Load(r, top + 9).y);
    V hdr = Load(r, top + 11);
    std::uint16_t first = (std::uint16_t)(top + Half(hdr.x)), end = (std::uint16_t)(top + Half(hdr.y));
    std::uint16_t colours = (std::uint16_t)(top + Half(Load(r, top + 12).w));
    int n = Count(first, end, 4);
    if ((kind == 1 || kind == 2 || kind == 3) && n == 0)
        return false;
    r.Qword(top + 13)[3] = FromBits(1);
    if (kind == 2 || kind == 3) {   // 0x0D2
        V v02 = Load(r, 3), v03 = Load(r, top + 13);
        v02.z = Sub(0.0f, 0.0f);
        v02.w = Sub(1.0f, 1.0f);
        float y = (float)(std::int32_t)Bits(v03.y) / 32768.0f;
        st.vf[3][1] = y;
        v02.y = Add(v02.y, y);
        SetVf(st, 2, v02);
        for (int k = 0; k < n; k++) {
            std::memset(r.Qword((std::uint16_t)(colours + k)), 0, 12);
            Store(r, (std::uint16_t)(first + 4 * k + 2), v02);
        }
        st.pc = 0x0E1;
        return true;
    }
    if (kind != 1) {
        r.Qword(top + 13)[3] = FromBits(0);
        st.pc = 0x05F;
        return true;
    }
    // 0x0E1: alpha over 255 into quadword 2's xy, the colour times 3 over its base's sum
    float (*vf)[4] = st.vf;
    st.i = FromBits(0x3B808081u);
    vf[4][0] = Add(0.0f, st.i);
    vf[4][1] = Add(0.0f, st.i);
    st.i = 3.0f;
    vf[6][0] = Add(0.0f, st.i);
    auto loadXyz = [&](int reg, std::uint16_t a) { std::memcpy(vf[reg], r.Qword(a), 12); };
    auto sum = [&]() {
        float acc = Add(vf[7][0], vf[7][1]);
        vf[7][0] = Add(acc, Mul(vf[1][0], vf[7][2]));
    };
    loadXyz(8, (std::uint16_t)(first + 1));
    for (int c = 0; c < 3; c++)
        vf[7][c] = Itof0(vf[8][c]);
    vf[9][3] = r.Qword((std::uint16_t)(first + 2))[3];
    std::memcpy(vf[10], r.Qword(colours), 16);
    sum();
    loadXyz(8, (std::uint16_t)(first + 5));
    auto div = [&]() {
        float a = In(vf[6][0]), b = In(vf[7][0]);
        if (b == 0.0f) {
            st.status |= a == 0.0f ? 0x410 : 0x820;
            return (std::signbit(a) != std::signbit(b)) ? -3.4028235e38f : 3.4028235e38f;
        }
        return core::Fix(a / b);
    };
    float q = div();
    std::uint16_t at = first, col = colours;
    do {
        vf[2][3] = Itof0(vf[9][3]);
        for (int c = 0; c < 4; c++)
            vf[3][c] = Itof0(vf[10][c]);
        for (int c = 0; c < 3; c++)
            vf[7][c] = Itof0(vf[8][c]);
        at = (std::uint16_t)(at + 4);
        vf[5][1] = Mul(vf[4][1], vf[2][3]);
        vf[5][0] = Mul(vf[4][0], vf[3][3]);
        for (int c = 0; c < 3; c++)
            vf[3][c] = Mul(vf[3][c], q);
        vf[9][3] = r.Qword((std::uint16_t)(at + 2))[3];
        std::memcpy(vf[10], r.Qword((std::uint16_t)(col + 1)), 16);
        sum();
        loadXyz(8, (std::uint16_t)(at + 5));
        float* q2 = r.Qword((std::uint16_t)(at - 2));
        q2[0] = vf[5][0];
        q2[1] = vf[5][1];
        std::memcpy(r.Qword(col), vf[3], 12);
        col = (std::uint16_t)(col + 1);
        q = div();
    } while (at != end);
    st.q = q;
    st.pc = 0x0FE;
    return true;
}

// --- 0x1DE of chunk 6744EAB9: a spot light.
//
// From TOP: 28-31 made from 0-3 and itself (0x22B) and used to project; 27 the spot's direction
// (turned by 4-6), 22-24 as for the other lights, 25 and 26 the cone's offset and scale over the
// screen radius squared. Each vertex: projected and tested against the guard band (outside, it is
// stored back unlit); the cone factor (the clamped fields of 26 * (|x|, |y|, r^2, w) + 25,
// multiplied), the cosine with the normal as d + d^3, the light clamped and times the cone added
// to quadword 2 (its z as the register held it) and through 22 to the colour. When 9.y is 4,
// 0x154 takes the cone at the object's centre (10) to scale 23 and 24 (or makes 23 23.0 when the
// centre is outside, and ends) and the light continues four-wide at 0x0FE.
bool SpotLight(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    float (*vf)[4] = st.vf;
    auto g = [&](int k) { V x = {}; std::memcpy(&x, vf[k], 16); return x; };
    auto xyz = [&](int k, const V& x) { std::memcpy(vf[k], &x, 12); };
    // the cone factor of the projected point in reg (q applied to xy), from scale and offset
    auto cone = [&](int reg, int sq, int scale, int offset, int out) {
        V p = g(reg);
        p.x = std::fabs(In(p.x));
        p.y = std::fabs(In(p.y));
        vf[sq][0] = Mul(p.x, p.x);
        vf[sq][1] = Mul(p.y, p.y);
        vf[sq][0] = Add(vf[sq][0], vf[sq][1]);
        p.z = Add(0.0f, vf[sq][0]);
        vf[reg][0] = p.x;
        vf[reg][1] = p.y;
        vf[reg][2] = p.z;
        V c;
        c.v = Add4(Mul4(g(scale).v, g(reg).v), Mul4(g(offset).v, Splat(1.0f)));
        for (int k = 0; k < 4; k++)
            c[k] = Max(Mini(c[k], 1.0f), 0.0f);
        c.x = Mul(c.x, c.w);
        c.x = Mul(c.x, c.z);
        c.x = Mul(c.x, c.y);
        std::memcpy(vf[out], &c, 16);
    };
    if (Half(Load(r, top + 9).y) == 4) {   // 0x154
        V hdr = Load(r, top + 11);
        std::uint16_t span = (std::uint16_t)(Half(hdr.y) - Half(hdr.x));
        if (span == 0 || span > 0x400)
            return false;
        Regs R;
        std::memcpy(R.vf, vf, sizeof R.vf);
        float (*rv)[4] = R.vf;
        auto rg = [&](int k) { V x = {}; std::memcpy(&x, rv[k], 16); return x; };
        std::memcpy(rv[2], r.Qword(top + 10), 12);
        for (int k = 0; k < 4; k++)
            std::memcpy(rv[3 + k], r.Qword((std::uint16_t)(top + 28 + k)), 16);
        V c = rg(2);
        F4 acc = Mul4(rg(3).v, Splat(c.x));
        acc = Add4(acc, Mul4(rg(4).v, Splat(c.y)));
        acc = Add4(acc, Mul4(rg(5).v, Splat(c.z)));
        std::memcpy(rv[7], r.Qword(top + 27), 12);
        std::memcpy(rv[8], r.Qword(top + 25), 16);
        std::memcpy(rv[9], r.Qword(top + 26), 16);
        V v12;
        v12.v = Add4(acc, Mul4(rg(6).v, Splat(1.0f)));
        std::memcpy(rv[12], &v12, 16);
        std::memcpy(rv[10], r.Qword(top + 23), 12);
        std::memcpy(rv[11], r.Qword(top + 24), 12);
        st.clip = ((st.clip << 6) | ClipCode(v12)) & 0xFFFFFF;
        float q = Reciprocal(r, v12.w);
        st.q = q;
        if (st.clip & 0x3F) {   // 0x179: the centre outside
            st.i = 23.0f;
            for (int k = 0; k < 3; k++)
                rv[10][k] = Add(0.0f, st.i);
            std::memcpy(r.Qword(top + 23), rv[10], 12);
            std::memcpy(vf, R.vf, sizeof R.vf);
            st.pc = 0x17E;
            return true;
        }
        rv[12][0] = Mul(rv[12][0], q);
        rv[12][1] = Mul(rv[12][1], q);
        // the cone, on R
        {
            float (*save)[4] = vf;
            vf = rv;
            cone(12, 13, 9, 8, 14);
            vf = save;
        }
        for (int k = 0; k < 3; k++) {
            rv[10][k] = Mul(rv[10][k], rv[14][0]);
            rv[11][k] = Mul(rv[11][k], rv[14][0]);
        }
        std::memcpy(r.Qword(top + 25), rv[7], 12);
        std::memcpy(r.Qword(top + 23), rv[10], 12);
        std::memcpy(r.Qword(top + 24), rv[11], 12);
        if (!LightFourWide(r, R))
            return false;   // not reached: the span was checked first
        std::memcpy(vf, R.vf, sizeof R.vf);
        return true;
    }
    V hdr = Load(r, top + 11);
    std::uint16_t first = (std::uint16_t)(top + Half(hdr.x));
    int n = Count(first, (std::uint16_t)(top + Half(hdr.y)), 4);
    if (n == 0)
        return false;
    MatrixProduct(r, vf, (std::uint16_t)(top + 28), top, (std::uint16_t)(top + 28));
    for (int k = 0; k < 4; k++)
        SetVf(st, 2 + k, Load(r, (std::uint16_t)(top + 28 + k)));
    std::uint16_t colours = (std::uint16_t)(top + Half(Load(r, top + 12).w));
    xyz(9, Load(r, top + 27));
    for (int k = 0; k < 3; k++)
        SetVf(st, 14 + k, Load(r, (std::uint16_t)(top + 4 + k)));
    {
        V d = g(9);
        F4 acc = Mul4(g(14).v, Splat(d.x));
        acc = Add4(acc, Mul4(g(15).v, Splat(d.y)));
        V t;
        t.v = Add4(acc, Mul4(g(16).v, Splat(d.z)));
        xyz(9, t);
    }
    xyz(6, Load(r, top + 22));
    xyz(7, Load(r, top + 24));
    xyz(8, Load(r, top + 23));
    float p = Erleng(g(9));
    vf[28][0] = p;
    {
        V u;
        u.v = Mul4(g(9).v, Splat(p));
        xyz(9, u);
    }
    vf[10][0] = Add(0.0f, vf[9][1]);
    vf[11][0] = Add(0.0f, vf[9][2]);
    SetVf(st, 13, Load(r, top + 25));
    SetVf(st, 12, Load(r, top + 26));
    for (int k = 0; k < n; k++) {
        std::uint16_t at = (std::uint16_t)(first + 4 * k);
        xyz(17, Load(r, (std::uint16_t)(at + 3)));
        V v19 = Load(r, (std::uint16_t)(at + 2));
        vf[19][0] = v19.x;
        vf[19][1] = v19.y;
        xyz(20, Load(r, (std::uint16_t)(colours + k)));
        SetVf(st, 18, Load(r, at));
        V pos = g(17);
        F4 acc = Mul4(g(2).v, Splat(pos.x));
        acc = Add4(acc, Mul4(g(3).v, Splat(pos.y)));
        acc = Add4(acc, Mul4(g(4).v, Splat(pos.z)));
        V v21;
        v21.v = Add4(acc, Mul4(g(5).v, Splat(1.0f)));
        SetVf(st, 21, v21);
        st.clip = ((st.clip << 6) | ClipCode(v21)) & 0xFFFFFF;
        float q = Reciprocal(r, v21.w);
        st.q = q;
        if (!(st.clip & 0x3F)) {
            vf[21][0] = Mul(vf[21][0], q);
            vf[21][1] = Mul(vf[21][1], q);
            cone(21, 27, 12, 13, 22);
            float a = Mul(vf[9][0], vf[18][0]);
            a = Add(a, Mul(vf[10][0], vf[18][1]));
            vf[25][0] = Add(a, Mul(vf[11][0], vf[18][2]));
            vf[26][0] = Mul(vf[25][0], vf[25][0]);
            a = Add(vf[25][0], 0.0f);
            vf[25][0] = Add(a, Mul(vf[26][0], vf[25][0]));
            V lit;
            lit.v = Add4(Add4(Splat(0.0f), g(7).v), Mul4(g(8).v, Splat(vf[25][0])));
            for (int c = 0; c < 3; c++)
                lit[c] = Mini(Max(lit[c], 0.0f), 1.0f);
            for (int c = 0; c < 3; c++)
                lit[c] = Mul(lit[c], vf[22][0]);
            xyz(23, lit);
            vf[19][0] = Add(vf[19][0], lit.x);
            vf[19][1] = Add(vf[19][1], lit.y);
            V col;
            col.v = Add4(Add4(Splat(0.0f), g(20).v), Mul4(g(6).v, Splat(lit.z)));
            xyz(20, col);
        }
        std::memcpy(r.Qword((std::uint16_t)(at + 2)), vf[19], 12);
        std::memcpy(r.Qword((std::uint16_t)(colours + k)), vf[20], 12);
    }
    st.p = p;
    st.pc = 0x22B;
    return true;
}

// --- 0x000 of chunk 19FE9C91 (set B): particles as upright quads.
//
// As 0x110 without the rotation: each particle's size, frame, alpha and colour blend from
// quadratics in its distance (here each weighted through vf01's fields), its centre through 7-10
// and the matrix at 4-7 of data memory, the corners the centre plus and minus 5's xy times the
// size; a quad with a corner outside the guard band marks its first two vertices (ADC), else its
// corners go to the screen; the STQs are vf01's fields and zeros. The packet at 48 is kicked.
bool Quads(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V h0 = Load(r, top);
    std::uint16_t vi06 = (std::uint16_t)(Half(h0.x) + top), vi07 = (std::uint16_t)(Half(h0.y) + top);
    std::uint16_t span = (std::uint16_t)(vi07 - vi06);
    if (span == 0 || span % 10 || span > 0x400)
        return false;
    float (*vf)[4] = st.vf;
    auto g = [&](int k) { V x = {}; std::memcpy(&x, vf[k], 16); return x; };
    auto set = [&](int k, const V& x) { std::memcpy(vf[k], &x, 16); };
    const std::uint16_t vi05 = 32768;
    set(2, Load(r, 0));
    set(3, Load(r, 1));
    set(4, Load(r, (std::uint16_t)(top + 5)));
    std::uint16_t vi09 = (std::uint16_t)(Half(Load(r, (std::uint16_t)(top + 6)).z) - 1);
    std::uint16_t vi08 = Half(Load(r, (std::uint16_t)(top + 6)).x);
    for (int k = 0; k < 4; k++)
        set(5 + k, Load(r, (std::uint16_t)(top + 1 + k)));
    r.Qword((std::uint16_t)(vi06 - 1))[0] = FromBits((std::uint16_t)(Half(h0.z) + vi05));
    for (int k = 0; k < 4; k++)
        set(13 + k, Load(r, (std::uint16_t)(4 + k)));
    vf[10][3] = Add(1.0f, 0.0f);
    for (int c = 0; c < 3; c++)
        vf[11][c] = Add(0.0f, 0.0f);
    for (int k = 0; k < 4; k++)
        set(28 + k, Load(r, (std::uint16_t)(top + 7 + k)));
    const float* one = vf[1];
    float acc[4];
    do {
        set(17, Load(r, (std::uint16_t)(vi06 + 3)));
        vf[19][3] = Sub(vf[4][3], vf[17][3]);
        vf[18][0] = one[0];
        vf[18][1] = Add(0.0f, vf[19][3]);
        vf[18][2] = Add(0.0f, vf[19][3]);
        vf[18][2] = Mul(vf[18][2], vf[18][2]);
        std::uint16_t vi11 = Half(Load(r, (std::uint16_t)(vi06 + 2)).x);
        auto times18 = [&](int dst, int src) {
            for (int c = 0; c < 3; c++)
                vf[dst][c] = Mul(vf[src][c], vf[18][c]);
        };
        times18(27, 5);
        vf[12][1] = one[1];
        vf[12][2] = one[2];
        times18(26, 8);
        vf[12][0] = 0.0f;   // vf00.x
        times18(25, 7);
        times18(24, 6);
        Store(r, (std::uint16_t)(vi06 + 4), g(1));
        acc[1] = Add(vf[27][1], vf[27][0]);
        vf[9][1] = Add(acc[1], Mul(one[1], vf[27][2]));
        Store(r, (std::uint16_t)(vi06 + 1), g(12));
        acc[2] = Add(vf[26][2], vf[26][0]);
        vf[12][0] = 0.0f;
        vf[12][1] = 0.0f;
        vf[9][2] = Add(acc[2], Mul(one[2], vf[26][1]));
        acc[3] = Mul(one[3], vf[25][0]);
        acc[3] = Add(acc[3], Mul(one[3], vf[25][1]));
        vf[9][3] = Add(acc[3], Mul(one[3], vf[25][2]));
        Store(r, (std::uint16_t)(vi06 + 6), g(12));
        vf[9][1] = Mini(vf[9][1], 1.0f);
        vf[9][2] = Mini(vf[9][2], 1.0f);
        vf[12][0] = one[0];
        acc[0] = Add(vf[24][0], vf[24][1]);
        vf[9][0] = Add(acc[0], Mul(one[0], vf[24][2]));
        vf[9][1] = Max(vf[9][1], 0.0f);
        vf[9][2] = Max(vf[9][2], 0.0f);
        Store(r, (std::uint16_t)(vi06 + 8), g(12));
        vf[9][3] = Ftoi0(vf[9][3]);
        std::uint16_t vi01 = Half(Load(r, (std::uint16_t)(vi06 + 2)).y);
        {
            V p = g(17);
            F4 a = Mul4(g(28).v, Splat(p.x));
            a = Add4(a, Mul4(g(29).v, Splat(p.y)));
            a = Add4(a, Mul4(g(30).v, Splat(p.z)));
            V o;
            o.v = Add4(a, Mul4(g(31).v, Splat(1.0f)));
            set(10, o);
        }
        std::uint16_t vi10 = (std::uint16_t)((vi01 + Half(vf[9][3])) & vi09);
        vi01 = (std::uint16_t)(top - vi11);
        {
            V p = g(10);
            F4 a = Mul4(g(13).v, Splat(p.x));
            a = Add4(a, Mul4(g(14).v, Splat(p.y)));
            a = Add4(a, Mul4(g(15).v, Splat(p.z)));
            V o;
            o.v = Add4(a, Mul4(g(16).v, Splat(p.w)));
            set(21, Load(r, (std::uint16_t)(vi01 + 47)));
            set(10, o);
        }
        if (vi08 & 1) {
            V c = g(21);
            for (int k = 0; k < 4; k++)
                c[k] = Itof0(c[k]);
            set(21, c);
            vf[19][0] = Sub(one[0], vf[18][0]);
            vi01 = (std::uint16_t)(vi01 - 1);
            F4 a = Mul4(g(21).v, Splat(vf[19][0]));
            V n = Load(r, (std::uint16_t)(vi01 + 47));
            for (int k = 0; k < 4; k++)
                n[k] = Itof0(n[k]);
            n.v = Add4(a, Mul4(n.v, Splat(vf[9][2])));
            for (int k = 0; k < 4; k++)
                n[k] = Ftoi0(n[k]);
            set(21, n);
        }
        vf[11][0] = Mul(vf[4][0], vf[9][0]);
        vf[11][1] = Mul(vf[4][1], vf[9][0]);
        float q = Reciprocal(r, vf[10][3]);
        st.q = q;
        vf[21][3] = Itof0(vf[21][3]);
        vf[19][0] = Load(r, (std::uint16_t)(top + 11)).x;
        vf[25][0] = Add(vf[10][0], vf[11][0]);
        for (int c = 0; c < 3; c++)
            vf[24][c] = Sub(vf[10][c], vf[11][c]);
        vf[25][1] = Sub(vf[10][1], vf[11][1]);
        vf[25][2] = Sub(vf[10][2], vf[11][2]);
        vf[21][3] = Mul(vf[21][3], vf[9][1]);
        vi10 = (std::uint16_t)(vi10 + top);
        vf[26][0] = Sub(vf[10][0], vf[11][0]);
        for (int c = 0; c < 3; c++)
            vf[27][c] = Add(vf[10][c], vf[11][c]);
        vf[26][1] = Add(vf[10][1], vf[11][1]);
        vf[26][2] = Add(vf[10][2], vf[11][2]);
        vf[21][3] = Mul(vf[21][3], vf[19][0]);
        set(20, Load(r, (std::uint16_t)(vi10 + 15)));
        std::uint32_t clip = st.clip;
        for (int k = 0; k < 4; k++) {
            V c = g(24 + k);
            c.w = vf[10][3];
            clip = ((clip << 6) | ClipCode(c)) & 0xFFFFFF;
        }
        st.clip = clip;
        vf[21][3] = Ftoi0(vf[21][3]);
        Store(r, vi06, g(20));
        for (int c = 0; c < 3; c++)
            vf[23][c] = Mul(vf[2][c], q);
        Store(r, (std::uint16_t)(vi06 + 2), g(21));
        if (clip) {
            for (int k = 0; k < 4; k++)
                r.Qword((std::uint16_t)(vi06 + 3 + 2 * k))[3] = FromBits(vi05);
        } else {
            const std::uint32_t wbits[4] = {0xFFFF8000u, 0xFFFF8000u, 0u, 0u};
            for (int k = 0; k < 4; k++) {
                for (int c = 0; c < 3; c++)
                    vf[24 + k][c] = Add(Mul(vf[23][c], vf[24 + k][c]), Mul(one[c], vf[3][c]));
                vf[24 + k][3] = FromBits(wbits[k]);
            }
            for (int k = 0; k < 4; k++)
                Store(r, (std::uint16_t)(vi06 + 3 + 2 * k), g(24 + k));
        }
        vi06 = (std::uint16_t)(vi06 + 10);
    } while (vi06 != vi07);
    Kick(r, (std::uint16_t)(top + 48));
    st.pc = 0x073;
    return true;
}

// --- 0x45C of chunk 2DDC9AA2 (from 0x477 with vi05 = 13.x - 1, and 0x47B with 0): a morph
// target added. The weight is 11.w + 22.x's x; when it is 0 (the Z flag, read four instructions
// on) nothing is added, though the first delta is still loaded in the branch's delay slot. Each
// vertex's quadword at vi05 (stride 13.x, from 11.x to 11.y) gains the delta at 12.w times it.
bool Morph(Runner& r, std::uint16_t offset, std::uint32_t endPc)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V h11 = Load(r, top + 11);
    std::uint16_t step = Half(Load(r, top + 13).x);
    std::uint16_t base = (std::uint16_t)(top + offset);
    std::uint16_t first = (std::uint16_t)(base + Half(h11.x)), end = (std::uint16_t)(base + Half(h11.y));
    std::uint16_t deltas = (std::uint16_t)(top + Half(Load(r, top + 12).w));
    std::uint16_t weightAt = (std::uint16_t)(top + Half(h11.w) + Half(Load(r, top + 22).x));
    V w = Load(r, weightAt);
    const bool zero = Add(w.x, 0.0f) == 0.0f;
    int n = Count(first, end, step);
    if (!zero && (n == 0 || step > 64))
        return false;
    std::memcpy(st.vf[3], &w, 12);
    SetVf(st, 4, Load(r, deltas));
    if (!zero) {
        V v02;
        std::memcpy(&v02, st.vf[2], 16);
        V d;
        for (int k = 0; k < n; k++) {
            d = Load(r, (std::uint16_t)(deltas + k));
            std::uint16_t at = (std::uint16_t)(first + step * k);
            V p = Load(r, at);
            for (int c = 0; c < 3; c++)
                d[c] = Mul(d[c], w.x);
            for (int c = 0; c < 3; c++)
                v02[c] = Add(p[c], d[c]);
            std::memcpy(r.Qword(at), &v02, 12);
        }
        SetVf(st, 4, d);
        SetVf(st, 2, v02);
    }
    st.pc = endPc;
    return true;
}

bool MorphFromStride(Runner& r)
{
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    return Morph(r, (std::uint16_t)(Half(Load(r, top + 13).x) - 1), 0x477);
}
bool MorphFromStart(Runner& r) { return Morph(r, 0, 0x477); }

// --- 0x47E of chunk 2DDC9AA2: a batch's normals made unit length and laid out four at a time,
// as 0x411 without the matrices: the loop pipelined two deep, MFP reading the ERLENG of two
// passes back (each ERLENG waits for the one before), the first two passes on what an earlier
// program left in vf29, vf30 and P.
bool UnitNormals(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V hdr = Load(r, top + 11);
    std::uint16_t vi05 = (std::uint16_t)(top + Half(hdr.x)), vi06 = (std::uint16_t)(top + Half(hdr.y) + 8);
    std::uint16_t span = (std::uint16_t)(vi06 - vi05);
    if (span == 0 || (span & 3) || span > 0x400)
        return false;
    float (*vf)[4] = st.vf;
    auto g = [&](int k) { V x = {}; std::memcpy(&x, vf[k], 16); return x; };
    auto set = [&](int k, const V& x) { std::memcpy(vf[k], &x, 16); };
    std::memcpy(vf[2], r.Qword(vi05), 12);
    V zero;
    zero.v = F4{Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(0.0f, 0.0f), Sub(1.0f, 1.0f)};
    set(28, zero);
    int vi07 = 6;
    float visible = st.p, pending = 0.0f;
    bool havePending = false;
    auto mr32 = [&](int k) {
        V x = g(k), y;
        for (int c = 0; c < 4; c++)
            y[c] = x[(c + 1) & 3];
        set(k, y);
    };
    do {
        mr32(25);
        mr32(26);
        mr32(27);
        vf[24][3] = visible;
        vi07--;
        vf[25][3] = Mul(vf[24][3], vf[30][0]);
        vf[26][3] = Mul(vf[24][3], vf[30][1]);
        vf[27][3] = Mul(vf[24][3], vf[30][2]);
        vi05 = (std::uint16_t)(vi05 + 4);
        if (vi07 == 0) {
            Store(r, (std::uint16_t)(vi05 - 12), g(28));
            Store(r, (std::uint16_t)(vi05 - 24), g(25));
            Store(r, (std::uint16_t)(vi05 - 20), g(26));
            Store(r, (std::uint16_t)(vi05 - 16), g(27));
            vi07 = 4;
        }
        set(30, g(29));
        set(29, g(2));
        std::memcpy(vf[2], r.Qword(vi05), 12);
        if (havePending)
            visible = pending;   // done while this ERLENG waits for it
        pending = Erleng(g(29));
        havePending = true;
    } while (vi05 != vi06);
    st.p = pending;
    if (vi07 - 4 < 0) {
        do {
            mr32(25);
            mr32(26);
            mr32(27);
            vi07--;
            vi05 = (std::uint16_t)(vi05 + 4);
        } while (vi07 != 0);
        Store(r, (std::uint16_t)(vi05 - 24), g(25));
        Store(r, (std::uint16_t)(vi05 - 20), g(26));
        Store(r, (std::uint16_t)(vi05 - 16), g(27));
        Store(r, (std::uint16_t)(vi05 - 12), g(28));
    }
    st.pc = 0x4A9;
    return true;
}

// --- 0x4F8 of chunk 2DDC9AA2: 22.xy added to each vertex's first quadword (stride 13.x).
bool OffsetXy(Runner& r)
{
    State& st = r.s;
    std::uint16_t top = (std::uint16_t)r.ctx.top;
    V h11 = Load(r, top + 11);
    std::uint16_t step = Half(Load(r, top + 13).x);
    std::uint16_t first = (std::uint16_t)(top + Half(h11.x)), end = (std::uint16_t)(top + Half(h11.y));
    std::uint16_t last = (std::uint16_t)(end - step), span = (std::uint16_t)(last - first);
    if (step == 0 || step > 64 || span % step || span / step > 1024)
        return false;
    int n = span / step + 1;
    V off = Load(r, top + 22);
    st.vf[3][0] = off.x;
    st.vf[3][1] = off.y;
    for (int k = 0; k < n; k++) {
        float* q = r.Qword((std::uint16_t)(first + step * k));
        st.vf[4][0] = Add(q[0], off.x);
        st.vf[4][1] = Add(q[1], off.y);
        q[0] = st.vf[4][0];
        q[1] = st.vf[4][1];
    }
    st.pc = 0x507;
    return true;
}

}  // namespace

// Every VF register but vf00 and vf01, and P: what the lights' handlers are compared on.
#define VF_2_TO_9 "vf02.xyzw vf03.xyzw vf04.xyzw vf05.xyzw vf06.xyzw vf07.xyzw vf08.xyzw vf09.xyzw "
#define VF_10_TO_19 "vf10.xyzw vf11.xyzw vf12.xyzw vf13.xyzw vf14.xyzw vf15.xyzw vf16.xyzw vf17.xyzw vf18.xyzw vf19.xyzw "
#define VF_20_TO_31 "vf20.xyzw vf21.xyzw vf22.xyzw vf23.xyzw vf24.xyzw vf25.xyzw vf26.xyzw vf27.xyzw vf28.xyzw vf29.xyzw " \
                    "vf30.xyzw vf31.xyzw "
const char kAllVfAndP[] = VF_2_TO_9 VF_10_TO_19 VF_20_TO_31 "P";

// Before a program: the batch's transform done in memory unless the program is one known not to
// read it. Those are the batch's own kicks, and the programs that make the next batch ready (its
// matrix, its header from the other half, its lights, colours, skinning, morphs) or transform it.
// Every other program may read the screen positions of the batch before: the outlines and the
// sprites (0x076: the shadows under characters) do, and a program on the microcode may read anything.
void BeforeProgram(Handler fn, core::Runner& r)
{
    if (!g_deferred.on)
        return;
    static const Handler kSafe[] = {BatchFinish, BatchSecond, UnlitFinish, MatrixAt14, CopyFromOtherBuffer, BatchSetup,
                                    DirectionalLight, PointLight, SpotLight, LitColours, Skin, SkinNormals, UnitNormals,
                                    MorphFromStride, MorphFromStart, OffsetXy, LitBatchTransform, BatchTransform274,
                                    BatchTransform28E};
    for (Handler h : kSafe)
        if (fn == h)
            return;
    Materialise(r);
}

void LeftToMicrocode(core::Runner& r) { Materialise(r); }

void DataWritten(std::uint32_t qaddr, std::uint32_t count, std::uint32_t cycle, std::uint32_t write)
{
    Deferred& d = g_deferred;
    if (!d.on || d.overwritten)
        return;
    static const bool seq = std::getenv("SLY1_VU1_SEQ") != nullptr;
    // what the transform reads of each vertex: its STQ (quadword 0) and its position (the last);
    // the header is kept (Materialise)
    const std::uint32_t stride = (std::uint32_t)d.stride, span = (std::uint16_t)(d.end - d.first);
    bool skipping = write != 0 && cycle > write;
    {   // an upload wholly outside the batch (most of them): nothing to look at
        std::uint32_t reach = skipping ? ((count + write - 1) / write) * cycle : count;
        std::uint32_t from = (qaddr - d.first) & 0x3FF;
        if (from >= span && from + reach <= 1024)
            return;
        // one block a vertex (a cycle of the batch's stride): the same quadwords of every vertex,
        // none of them its STQ or position (the colours' uploads)
        if (skipping && cycle == stride && from + reach <= 1024) {
            bool touches = false;
            for (std::uint32_t q = 0; q < write; q++) {
                std::uint32_t slot = (from + q) % stride;
                touches = touches || slot == 0 || slot == stride - 1;
            }
            if (!touches)
                return;
        }
    }
    for (std::uint32_t k = 0; k < count; k++) {
        std::uint32_t at = skipping ? qaddr + (k / write) * cycle + (k % write) : qaddr + k;
        std::uint32_t rel = (at - d.first) & 0x3FF;
        if (rel < span && (rel % stride == 0 || rel % stride == stride - 1)) {
            d.overwritten = true;
            if (seq)
                std::fprintf(stderr, "seq   unpack over the batch: %u quadwords at %u (cycle %u, write %u)\n", count, qaddr, cycle, write);
            return;
        }
    }
}

void BatchCounts(std::uint64_t* taken, std::uint64_t* notTaken)
{
    *taken = g_taken;
    *notTaken = g_notTaken;
    if (!g_lateBy.empty() || g_skippedOverwritten) {
        std::uint64_t all = 0;
        for (const auto& kv : g_lateBy)
            all += kv.second;
        std::fprintf(stderr, "vu1: %llu batches transformed late in memory (%llu vertices, %.3f s), %llu not (overwritten first); for:\n",
                     (unsigned long long)all, (unsigned long long)g_lateVertices, g_lateNs / 1e9, (unsigned long long)g_skippedOverwritten);
        for (const auto& kv : g_lateBy) {
            std::uint32_t hash = (std::uint32_t)(kv.first >> 12), entry = (std::uint32_t)(kv.first & 0xFFF);
            const char* name = "(on the microcode)";
            for (std::size_t k = 0; k < kHandlerCount; k++)
                if (kHandlers[k].chunkHash == hash && kHandlers[k].entry == entry)
                    name = kHandlers[k].name;
            std::fprintf(stderr, "vu1:   %08X 0x%03X %s: %llu\n", hash, entry, name, (unsigned long long)kv.second);
        }
    }
}

const HandlerEntry kHandlers[] = {
    {0x79A6F5FB, 0x356, LitBatchTransform, "lit batch transform",
     "vf02.xyzw vf03.xyzw vf04.xyzw vf05.xyzw vf06.xyzw vf07.xyzw vf08.xyzw vf09.xyzw vf10.xyzw vf11.xyzw "
     "vf12.xyzw vf14.xy vf15.xyzw vf16.xyzw vf17.xyzw vf19.xyzw vf20.xyzw vf21.xyzw vf22.xyzw vf24.xy "
     "vf25.xyzw vf26.xyzw vf27.xyzw vf29.xy clip"},
    {0x79A6F5FB, 0x34A, BatchTransform274, "batch transform (three quadwords)",
     "vf02.xyzw vf03.xyzw vf04.xyzw vf05.xyzw vf06.xyzw vf07.xyzw vf08.xyzw vf09.xyzw vf10.xyzw vf11.xyzw "
     "vf12.xyzw vf14.xy vf15.xyzw vf16.xyzw vf17.xyzw vf19.xyzw vf20.xyzw vf21.xyzw vf22.xyzw vf24.xy "
     "vf25.xyzw vf26.xyzw vf27.xyzw vf29.xy clip"},
    {0x79A6F5FB, 0x350, BatchTransform28E, "batch transform (three quadwords, far plane)",
     "vf02.xyzw vf03.xyzw vf04.xyzw vf05.xyzw vf06.xyzw vf07.xyzw vf08.xyzw vf09.xyzw vf10.xyzw vf11.xyzw "
     "vf12.xyzw vf14.xy vf15.xyzw vf16.xyzw vf17.xyzw vf19.xyzw vf20.xyzw vf21.xyzw vf22.xyzw vf24.xy "
     "vf25.xyzw vf26.xyzw vf27.xyzw vf29.xy clip"},
    {0xE1624411, 0x002, BatchFinish, "lit batch finish", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31 "Q I clip"},
    {0x6744EAB9, 0x17E, DirectionalLight, "directional light", kAllVfAndP},
    {0x6744EAB9, 0x1A6, PointLight, "point light", kAllVfAndP},
    {0xB90790F3, 0x242, LitColours, "lit colours",
     "vf02.xyzw vf03.xyzw vf04.xyzw vf05.xyzw vf06.xyzw vf07.xyzw vf08.xyzw vf09.xyzw vf10.xyzw vf11.xyzw "
     "vf12.xyzw vf13.xyzw vf14.xyzw vf15.xyzw vf16.xyzw I"},
    {0x2DDC9AA2, 0x401, Skin, "skinning",
     "vf02.xyz vf03.xyzw vf04.xyzw vf05.xyzw vf06.xyzw vf07.xyzw vf08.xyzw vf09.xyzw vf10.xyzw vf11.xyzw "
     "vf12.xyzw vf13.xyzw vf14.xyzw vf15.xyzw vf16.xyzw vf17.xyzw vf18.xyzw vf19.xyzw vf20.xyz vf21.xyz "
     "vf22.xyz vf23.xyz"},
    {0x7B92AF0F, 0x26D, Outlines, "outlines", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31 "P clip"},
    {0xE1624411, 0x076, Sprites, "sprites", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31 "Q I clip"},
    {0x1AE4A8D6, 0x110, Particles, "particles", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31 "Q P I clip"},
    {0x2DDC9AA2, 0x411, SkinNormals, "skinned normals", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31 "P"},
    {0x79A6F5FB, 0x318, MatrixAt14, "matrix at 14", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31},
    {0x79A6F5FB, 0x31F, CopyFromOtherBuffer, "copy from the other buffer", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31},
    {0xE1624411, 0x05F, UnlitFinish, "unlit batch finish", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31 "Q I clip"},
    {0xE1624411, 0x051, BatchSetup, "batch setup", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31 "Q I"},
    {0x6744EAB9, 0x1DE, SpotLight, "spot light", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31 "Q P I clip"},
    {0x19FE9C91, 0x000, Quads, "particle quads", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31 "Q clip"},
    {0x2DDC9AA2, 0x477, MorphFromStride, "morph (from the stride)", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31},
    {0x2DDC9AA2, 0x47B, MorphFromStart, "morph (from the start)", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31},
    {0x2DDC9AA2, 0x47E, UnitNormals, "unit normals", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31 "P"},
    {0x2DDC9AA2, 0x4F8, OffsetXy, "xy offset", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31},
    {0xE1624411, 0x01E, BatchSecond, "lit batch second pass", VF_2_TO_9 VF_10_TO_19 VF_20_TO_31 "Q I clip"},
};
const std::size_t kHandlerCount = sizeof kHandlers / sizeof kHandlers[0];

}  // namespace vu1
}  // namespace port
