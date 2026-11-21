// SPDX-License-Identifier: AGPL-3.0-or-later
// The game's hand-written assembly functions (sly1-decomp config/us/original_asm.tsv, marked
// ASM_FUNC in its sources), written in C++ for the port (docs/DESIGN.md, section 3).
//
// Each computes what retail's instructions compute, worked out from the function's role in the
// decompilation's name tables and from its instructions in the player's own disassembly (read,
// not copied): the same fields, the same order of every sum (the VU0 accumulator chains), the
// same rounding of each product before it is added, and VU0's float rules through
// port/vu0_impl.h (no infinity or NaN, denormals as zero, the square root of the absolute value,
// a reciprocal that saturates). A lane the hardware leaves as it was is kept; a lane it leaves
// undefined (no caller reads it) is 0. Fields are read at their retail offsets, which the port's
// layouts keep (docs/DESIGN.md, section 4); a pointer field is a 32-bit game address.
#include <cmath>
#include <cstring>

#include "common.h"
#include "placed_object.h"
#include "port/ptr32.h"
#include "port/retail_data.h"
#include "port/system.h"
#include "port/vu0_impl.h"

using namespace port_vu0;

typedef __int128 Quadword;
typedef unsigned __int128 Qword128;

struct Vec4;
struct TreeNode03;
struct TriggerZone;
struct Camera;
struct LookSource;
struct TargetCandidate;
struct World5ModeCollider;
struct Mat4A;
struct PointGraph;
struct Effect;
struct RangeRecord;
struct Quad_001B4580;
struct Collider;
struct CollisionMesh;
struct ImpulseRow;
struct FacePoint;
struct PlacedObject;
struct SwitchedCollider;
struct ObjectGatherer;
struct PathFollower;
struct BodyContact;

// The game's constants and globals these functions read, at their retail addresses in main memory
// (port/retail_data.h): unit vectors, the identity matrix, the largest-float constants, the
// quadratics of world 5's collider, the effect curve, the camera and player pointers.
#define GLOBAL(addr) ((u8*)port::Native(addr))

namespace {

// --- memory at retail offsets

inline u8* B(const void* p) { return (u8*)p; }
inline u128 Q(const void* p, u32 off = 0)
{
    u128 v;
    std::memcpy(&v, B(p) + off, 16);
    return v;
}
inline void PutQ(void* p, u32 off, u128 v) { std::memcpy(B(p) + off, &v, 16); }
inline f32 F(const void* p, u32 off)
{
    f32 v;
    std::memcpy(&v, B(p) + off, 4);
    return v;
}
inline void PutF(void* p, u32 off, f32 v) { std::memcpy(B(p) + off, &v, 4); }
inline u32 W(const void* p, u32 off)
{
    u32 v;
    std::memcpy(&v, B(p) + off, 4);
    return v;
}
inline void PutW(void* p, u32 off, u32 v) { std::memcpy(B(p) + off, &v, 4); }
inline u8* Ptr(const void* p, u32 off) { return (u8*)port::Native(W(p, off)); }

// --- VU0 and FPU sequences

const f32 kEpsilon = FromBits(0x38D1B717);   // 1e-4

// vmul.xyz, then x + y (vadday.x), then + 1.0 * z (vmaddz.x): the squared length or the dot of xyz.
inline f32 Dot3(L a, L b) { return port_vu0::Add(port_vu0::Add(Mul(a.v[0], b.v[0]), Mul(a.v[1], b.v[1])), Mul(a.v[2], b.v[2])); }
inline f32 Len2(L a) { return Dot3(a, a); }
// vsqrt: the root of the absolute value.
inline f32 Root(f32 x) { return Fix(std::sqrt(std::fabs(x))); }
// The FPU's division: no infinity, a zero divisor gives the largest float with the quotient's sign.
inline f32 FDiv(f32 a, f32 b)
{
    if (b == 0.0f)
        return FromBits(((Bits(a) ^ Bits(b)) & 0x80000000u) | 0x7F7FFFFFu);
    return Fix(a / b);
}
// The x lane of a transposed rotation, each row dotted with v, in the order the accumulator forms it:
// row 0 as (x + y) + z, row 1 as (y + x) + z, row 2 as (z + x) + y.
inline L RotateTransposedOrdered(L r0, L r1, L r2, L v)
{
    L out;
    out.v[0] = port_vu0::Add(port_vu0::Add(Mul(r0.v[0], v.v[0]), Mul(r0.v[1], v.v[1])), Mul(r0.v[2], v.v[2]));
    out.v[1] = port_vu0::Add(port_vu0::Add(Mul(r1.v[1], v.v[1]), Mul(r1.v[0], v.v[0])), Mul(r1.v[2], v.v[2]));
    out.v[2] = port_vu0::Add(port_vu0::Add(Mul(r2.v[2], v.v[2]), Mul(r2.v[0], v.v[0])), Mul(r2.v[1], v.v[1]));
    out.v[3] = 0;
    return out;
}
// xyz = r0 * v.x + r1 * v.y + r2 * v.z (the accumulator chain), w kept from v.
inline L RotateKeepW(L r0, L r1, L r2, L v)
{
    L out;
    for (int i = 0; i < 3; i++)
        out.v[i] = port_vu0::Add(port_vu0::Add(Mul(r0.v[i], v.v[0]), Mul(r1.v[i], v.v[1])), Mul(r2.v[i], v.v[2]));
    out.v[3] = v.v[3];
    return out;
}
// vopmula / vopmsub: a x b in xyz, w zeroed (vsub.w).
inline L Cross(L a, L b)
{
    L out;
    out.v[0] = port_vu0::Sub(Mul(a.v[1], b.v[2]), Mul(b.v[1], a.v[2]));
    out.v[1] = port_vu0::Sub(Mul(a.v[2], b.v[0]), Mul(b.v[2], a.v[0]));
    out.v[2] = port_vu0::Sub(Mul(a.v[0], b.v[1]), Mul(b.v[0], a.v[1]));
    out.v[3] = 0;
    return out;
}
inline L Scale(L a, f32 k)
{
    L out;
    for (int i = 0; i < 4; i++)
        out.v[i] = Mul(a.v[i], k);
    return out;
}
inline L AddL(L a, L b)
{
    L out;
    for (int i = 0; i < 4; i++)
        out.v[i] = port_vu0::Add(a.v[i], b.v[i]);
    return out;
}
inline L SubL(L a, L b)
{
    L out;
    for (int i = 0; i < 4; i++)
        out.v[i] = port_vu0::Sub(a.v[i], b.v[i]);
    return out;
}
// Normalised as VU0 does it: the length's square root, its saturating reciprocal, all four lanes.
inline L Normalised(L a) { return Scale(a, Reciprocal(Root(Len2(a)))); }
// p + vf0 (w gains 1) + c * k: a corner of a sphere's box.
inline L Corner(L p, L c, f32 k)
{
    L out;
    for (int i = 0; i < 4; i++)
        out.v[i] = port_vu0::Add(port_vu0::Add(p.v[i], i == 3 ? 1.0f : 0.0f), Mul(c.v[i], k));
    return out;
}
inline void MinXyz(L& lo, L v)
{
    for (int i = 0; i < 3; i++)
        if (v.v[i] < lo.v[i])
            lo.v[i] = v.v[i];
}
inline void MaxXyz(L& hi, L v)
{
    for (int i = 0; i < 3; i++)
        if (v.v[i] > hi.v[i])
            hi.v[i] = v.v[i];
}
// The MMI shuffle of two floats and a third into a quadword: (x, y, z, z's sign word).
inline u128 Shuffled(f32 x, f32 y, f32 z) { return St(Lanes(x, y, z, FromBits((Bits(z) & 0x80000000u) ? 0xFFFFFFFFu : 0))); }

inline f32 Clamp01(f32 x) { return x < 0.0f ? 0.0f : (1.0f < x ? 1.0f : x); }

}  // namespace

// --- vectors

u128 ScaleQuadword(u128 a, u128 b) { return MulX(b, a); }

static s32 Below(void* a, u32 threshold) { return Len2(Ld(Q(a))) < FromBits(threshold) ? 1 : 0; }
s32 IsVelocityNegligible(void* a) { return Below(a, 0x40800000); }          // 4.0
s32 IsSpinNegligible(void* a) { return Below(a, 0x39D1B717); }              // 0.0004
s32 IsAccelerationNegligible(void* a) { return Below(a, 0x41C80000); }      // 25.0
s32 IsSpinAccelerationNegligible(void* a) { return Below(a, 0x3B23D70B); }  // 0.0025

Quadword AddWeightedVectors(Quadword a, Quadword b, Quadword c, Quadword d)
{
    L la = Ld((u128)a), lb = Ld((u128)b), lc = Ld((u128)c), ld = Ld((u128)d);
    L out;
    for (int i = 0; i < 4; i++)
        out.v[i] = port_vu0::Add(Mul(lb.v[i], la.v[0]), Mul(ld.v[i], lc.v[0]));
    return (Quadword)St(out);
}

f32 MeasureListenerDistance(void* listener, void* position)
{
    return Root(Len2(SubL(Ld(Q(listener)), Ld(Q(position)))));
}

f32 SplitVectorAlongAxis(const Vec4* v, const Vec4* axis, Vec4* along, Vec4* rest)
{
    L lv = Ld(Q(v)), la = Ld(Q(axis));
    f32 d = Dot3(lv, la);
    L part = Scale(la, d);
    if (along)
        PutQ(along, 0, St(part));
    if (rest)
        PutQ(rest, 0, St(SubL(lv, part)));
    return d;
}

void BuildPerpendicularPair(Vec4* in, Vec4* out, const Vec4* a, const Vec4* b)
{
    L v = Ld(Q(in));
    L c = Cross(Ld(Q(a)), v);
    if (Len2(c) < kEpsilon)
        c = Cross(Ld(Q(b)), v);
    L d = Cross(v, c);
    PutQ(out, 0x00, St(Normalised(c)));
    PutQ(out, 0x10, St(Normalised(d)));
}

// --- matrices

u128 TransformVector4(void* m, u128 v) { return port_vu0::Transform4(Q(m, 0), Q(m, 0x10), Q(m, 0x20), Q(m, 0x30), v); }

s32 ScaleMatrix(Mat4A* out, const Mat4A* in, u128 scale)
{
    for (u32 r = 0; r < 4; r++)
        PutQ(out, r * 16, MulX(Q(in, r * 16), scale));
    return (s32)port::GameAddr(out);
}

Mat3 MultiplyByTransposedMatrix3x3(const Mat3& a, const Mat3& b)
{
    L a0 = Ld(Q(&a, 0)), a1 = Ld(Q(&a, 0x10)), a2 = Ld(Q(&a, 0x20));
    u128 rows[3];
    for (u32 i = 0; i < 3; i++)
        rows[i] = St(RotateTransposedOrdered(a0, a1, a2, Ld(Q(&b, i * 16))));
    Mat3 out;
    std::memcpy(&out, rows, sizeof rows);
    return out;
}

void OrthonormalizeMatrix(Mat3* m)
{
    L n0 = Normalised(Ld(Q(m, 0)));
    L r2 = Cross(n0, Ld(Q(m, 0x10)));
    L n2 = Normalised(r2);
    L n1 = Normalised(Cross(n2, n0));
    PutQ(m, 0x00, St(n0));
    PutQ(m, 0x10, St(n1));
    PutQ(m, 0x20, St(n2));
}

// --- placed objects: their velocity and spin through the rotation at +0xD0 (property accessors)

void GetPlacedObjectLocalVelocity(void* self, void* out)
{
    PutQ(out, 0, St(RotateTransposedOrdered(Ld(Q(self, 0xD0)), Ld(Q(self, 0xE0)), Ld(Q(self, 0xF0)), Ld(Q(self, 0x150)))));
}

void StorePlacedObjectLocalVelocity(void* self, void* in)
{
    PutQ(self, 0x150, St(RotateKeepW(Ld(Q(self, 0xD0)), Ld(Q(self, 0xE0)), Ld(Q(self, 0xF0)), Ld(Q(in)))));
}

void StorePlacedObjectLocalSpin(void* self, void* in)
{
    PutQ(self, 0x160, St(RotateKeepW(Ld(Q(self, 0xD0)), Ld(Q(self, 0xE0)), Ld(Q(self, 0xF0)), Ld(Q(in)))));
}

void ComputeChainedWorldFrame(void* a, void* pos, void* rot)
{
    PutQ(pos, 0, Q(GLOBAL(0x00248D30)));
    PutQ(rot, 0x00, Q(a, 0x50));
    PutQ(rot, 0x10, Q(a, 0x60));
    PutQ(rot, 0x20, Q(a, 0x70));
    u8* node = B(a);
    while (W(node, 0x80) != 0) {
        u8* parent = Ptr(node, 0x80);
        L r0 = Ld(Q(parent, 0x50)), r1 = Ld(Q(parent, 0x60)), r2 = Ld(Q(parent, 0x70));
        L t = RotateKeepW(r0, r1, r2, Ld(Q(node, 0x40)));
        L p = RotateKeepW(r0, r1, r2, Ld(Q(pos)));
        PutQ(pos, 0, St(AddL(p, t)));
        L n0 = RotateKeepW(r0, r1, r2, Ld(Q(rot, 0x00)));
        L n1 = RotateKeepW(r0, r1, r2, Ld(Q(rot, 0x10)));
        L n2 = RotateKeepW(r0, r1, r2, Ld(Q(rot, 0x20)));
        PutQ(rot, 0x00, St(n0));
        PutQ(rot, 0x10, St(n1));
        PutQ(rot, 0x20, St(n2));
        node = parent;
    }
    PutQ(pos, 0, St(AddL(Ld(Q(pos)), Ld(Q(node, 0x40)))));
}

// --- trees

s32 FindPlaneTreeNodeForPoint(Vec4* pos, TreeNode03* nodes)
{
    L p = Ld(Q(pos));
    u8* node = B(nodes);
    while (node) {
        u8* plane = Ptr(node, 0);
        f32 d = port_vu0::Sub(Dot3(p, Ld(Q(plane))), F(plane, 0x10));
        if (d < kEpsilon) {
            if (W(node, 4) == 0)
                return (s32)port::GameAddr(node);
            node = Ptr(node, 4);
        } else {
            node = Ptr(node, 8);
        }
    }
    return 0;
}

s32 IsPointInsideFaceTree(TreeNode03* self, FacePoint* value)
{
    u8* node = B(self);
    if (!node)
        return 0;
    L p = Ld(Q(value));
    for (;;) {
        u8* face = Ptr(node, 0);
        f32 d = port_vu0::Sub(Dot3(Ld(Q(face, 0x10)), p), F(face, 0));
        if (d < 0.0f) {
            node = Ptr(node, 4);
            if (!node)
                return 1;
        } else {
            node = Ptr(node, 8);
            if (!node)
                return 0;
        }
    }
}

void LookupValueTreeLeaf(void* a, void* b, void* c)
{
    u32 result = 0xFFFFFFF;
    if (a && W(a, 0x38) != 0) {
        u32 node = W(a, 0x38);
        if (!(node & 0x80000000u)) {
            L p = Ld(Q(b));
            for (;;) {
                u8* n = (u8*)port::Native(node);
                f32 d = port_vu0::Sub(Dot3(p, Ld(Q(n))), F(n, 0x10));
                node = d < 0.0f ? W(n, 0x14) : W(n, 0x18);
                if (node & 0x80000000u)
                    break;
            }
        }
        result = node & 0x7FFFFFFFu;
    }
    PutW(c, 0, result);
}

// --- nearest points

s32 FindNearestStoredPoint(TriggerZone* self, f32* out)
{
    f32 best = F(GLOBAL(0x00249DD0), 0);
    s32 n = (s32)W(self, 0x7B4);
    if (n == 0)
        return 0;
    L here = Ld(Q(self, 0x100));
    s32 index = 0;
    for (s32 k = n - 1; k >= 0; k--) {
        f32 d = Root(Len2(SubL(Ld(Q(self, 0x7C0 + (u32)k * 16)), here)));
        if (d < best) {
            best = d;
            index = k;
        }
    }
    if (out)
        *out = best;
    return index;
}

PlacedObject* FindNearestAgentKey(void* a, void* b)
{
    f32 best = F(GLOBAL(0x0024E600), 0);
    L p = Ld(Q(b));
    u8* result = nullptr;
    for (u32 i = 0; i < 6; i++) {
        u8* obj = Ptr(a, 0xC64 + i * 4);
        L d = SubL(Ld(Q(obj, 0x140)), p);
        f32 d2 = port_vu0::Add(Mul(d.v[0], d.v[0]), Mul(d.v[1], d.v[1]));
        if (d2 < best) {
            best = d2;
            result = obj;
        }
    }
    return (PlacedObject*)result;
}

s32 CompareDistanceFromPlayer(TargetCandidate* a, TargetCandidate* b)
{
    L player = Ld(Q(port::Native(W(GLOBAL(0x00262E10), 0)), 0x140));
    f32 da = Root(Len2(SubL(Ld(Q(a, 0x10)), player)));
    f32 db = Root(Len2(SubL(Ld(Q(b, 0x10)), player)));
    return da < db ? -1 : 1;
}

void MeasurePointGraphEdges(PointGraph* m)
{
    for (s32 i = 0; i < (s32)W(m, 8); i++) {
        u8* edge = Ptr(m, 0x10) + (u32)i * 12;
        L d = SubL(Ld(Q(Ptr(edge, 0))), Ld(Q(Ptr(edge, 4))));
        PutF(edge, 8, Root(port_vu0::Add(Mul(d.v[0], d.v[0]), Mul(d.v[1], d.v[1]))));
    }
}

// --- the camera

void BuildCameraGroundFrame(void* self)
{
    L flat = Lanes(-F(self, 0x80), -F(self, 0x84), 0, 0);
    f32 len = Root(Len2(flat));
    L forward = len < kEpsilon ? Ld(Q(GLOBAL(0x00248D00))) : Scale(flat, Reciprocal(len));
    L up = Ld(Q(GLOBAL(0x00248D20)));
    L side = Cross(up, forward);
    u128 keep = Q(self, 0xA0);
    L back80 = Scale(Ld(Q(self, 0x80)), -1.0f);
    L back90 = Scale(Ld(Q(self, 0x90)), -1.0f);
    PutQ(self, 0x3C0, keep);
    PutQ(self, 0x370, St(forward));
    PutQ(self, 0x380, St(side));
    PutQ(self, 0x390, St(up));
    PutQ(self, 0x3A0, St(back80));
    PutQ(self, 0x3B0, St(back90));
}

s32 ComputeCameraDistanceFade(Camera* self, u128 d, f32* fade, f32 radius, f32 x)
{
    f32 k = F(self, 0xB0);
    f32 d2 = Len2(Ld(d));
    f32 r = port_vu0::Add(Mul(x, k), radius);
    f32 outer = Mul(r, r);
    if (outer < d2)
        return 0;
    f32 e = port_vu0::Sub(r, Mul(k, 400.0f));
    f32 inner = Mul(e, e);
    *fade = inner < d2 ? FDiv(port_vu0::Sub(outer, d2), port_vu0::Sub(outer, inner)) : 1.0f;
    return 1;
}

void FillLookDirectionBlock(void* out, LookSource* src)
{
    PutF(out, 0x00, F(src, 0xC0));
    PutQ(out, 0x10, Q(src, 0xB0));
    PutF(out, 0x20, F(src, 0xC4));
    L dir = Ld(Q(src, 0xD0));
    f32 len = Root(Len2(dir));
    PutQ(out, 0x30, len < kEpsilon ? Q(GLOBAL(0x00248D20)) : St(Scale(dir, Reciprocal(len))));
    PutW(out, 0x40, W(src, 0xE0));
    PutW(out, 0x44, W(src, 0x7C));
}

// --- effects, range records, colours

void InitEffect(Effect* self, void* pos, f32 scale, s32 flags)
{
    PutW(self, 0x24, (u32)flags);
    PutF(self, 0x18, F(GLOBAL(0x0026185C), 0));   // the game clock's time
    PutF(self, 0x1C, 1.0f);
    PutF(self, 0x30, scale);
    PutQ(self, 0x40, Q(GLOBAL(0x0026B530)));
    u128 p = Q(pos);
    PutQ(self, 0x80, p);
    PutQ(self, 0x90, p);
    PutQ(self, 0x50, Q(GLOBAL(0x002483D0), 0x00));
    PutQ(self, 0x60, Q(GLOBAL(0x002483D0), 0x10));
    PutQ(self, 0x70, Q(GLOBAL(0x002483D0), 0x20));
    u8* owner = Ptr(self, 0xC);
    f32 d = Root(Len2(SubL(Ld(Q(self, 0x80)), Ld(Q(owner, 0x140)))));
    if (F(owner, 0x2A4) < d) {
        PutF(owner, 0x2A4, d);
        PutF(owner, 0x3D0, d);
        PutF(owner, 0x3CC, d);
        PutF(owner, 0x2A8, d);
    }
}

void InitRangeRecord(void* p)
{
    PutF(p, 0x04, -1.0f);
    PutF(p, 0x08, -1.0f);
    PutF(p, 0x40, 100.0f);
    PutF(p, 0x44, 400.0f);
    PutW(p, 0xB0, 0xFFFFFFFFu);
    PutQ(p, 0x30, Q(GLOBAL(0x00248D10)));
    PutQ(p, 0x20, St(Scale(Ld(Q(GLOBAL(0x00248D20))), -1.0f)));
}

s32 RangeRecordReachesSphere(RangeRecord* node, void* sphere, f32 radius)
{
    u8* owner = Ptr(node, 0);
    if (!owner)
        return 0;
    u32 mask = W(owner, 0xC);
    u32 cameraMask = W(port::Native(W(GLOBAL(0x00261990), 0)), 0x21C);
    if (!(mask & 0x10000000u) && (cameraMask & mask) != cameraMask)
        return 0;
    f32 r = port_vu0::Add(radius, F(node, 0x60));
    f32 d2 = Len2(SubL(Ld(Q(sphere)), Ld(Q(node, 0x50))));
    return Mul(r, r) < d2 ? 0 : 1;
}

void ConvertRgbToHsv(Quad_001B4580* p, f32* dst)
{
    f32 r = Clamp01(F(p, 0)), g = Clamp01(F(p, 4)), b = Clamp01(F(p, 8));
    f32 mx = g < r ? (b > r ? b : r) : (b > g ? b : g);
    f32 mn = r < g ? (b < r ? b : r) : (b < g ? b : g);
    f32 delta = port_vu0::Sub(mx, mn);
    f32 s = mx == 0.0f ? 0.0f : FDiv(delta, mx);
    f32 h;
    if (s == 0.0f) {
        h = -1.0f;
    } else {
        h = 0.0f;
        if (r == mx)
            h = FDiv(port_vu0::Sub(g, b), delta);
        else if (g == mx)
            h = port_vu0::Add(FDiv(port_vu0::Sub(b, r), delta), 2.0f);
        else if (b == mx)
            h = port_vu0::Add(FDiv(port_vu0::Sub(r, g), delta), 4.0f);
        h = Mul(h, 60.0f);
        if (h < 0.0f)
            h = port_vu0::Add(h, 360.0f);
    }
    u128 v = Shuffled(h, s, mx);
    std::memcpy(dst, &v, 16);
}

// --- colliders

void AddColliderForce(Collider* self, Vec4* force) { PutQ(self, 0x170, St(AddL(Ld(Q(force)), Ld(Q(self, 0x170))))); }
void AddColliderTorque(Collider* self, Vec4* torque) { PutQ(self, 0x180, St(AddL(Ld(Q(torque)), Ld(Q(self, 0x180))))); }

void ComputeColliderPointVelocity(Collider* body, Vec4* point, ImpulseRow* row, Vec4* out)
{
    L offset = SubL(Ld(Q(point)), Ld(Q(body, 0x140)));
    L spin = Cross(Ld(Q(row, 0x10)), offset);
    PutQ(out, 0, St(AddL(Ld(Q(row, 0)), spin)));
}

void ApplyColliderInverseInertia(Collider* self, Vec4* in, Vec4* out)
{
    L r0 = Ld(Q(self, 0x110)), r1 = Ld(Q(self, 0x120)), r2 = Ld(Q(self, 0x130));
    L local = RotateTransposedOrdered(r0, r1, r2, Ld(Q(in)));
    L scaled = RotateKeepW(Ld(Q(self, 0x320)), Ld(Q(self, 0x330)), Ld(Q(self, 0x340)), local);
    L world = RotateKeepW(r0, r1, r2, scaled);
    world.v[3] = 0;
    PutQ(out, 0, St(world));
}

void ExpandColliderBoundsBySphere(void* self, void* p, f32 f)
{
    L point = Ld(Q(p));
    f32 dist = Root(Len2(SubL(point, Ld(Q(self, 0x140)))));
    L unit = Ld(Q(GLOBAL(0x00248D40)));
    L lo = Ld(Q(self, 0x420));
    MinXyz(lo, Corner(point, unit, -f));
    PutQ(self, 0x420, St(lo));
    L hi = Ld(Q(self, 0x430));
    MaxXyz(hi, Corner(point, unit, f));
    PutQ(self, 0x430, St(hi));
    // As retail: a larger distance replaces the radius only when it is over 1; one that is not
    // larger replaces it whenever it is positive.
    f32 d = port_vu0::Add(dist, f);
    if (F(self, 0x3D0) < d) {
        if (1.0f < d)
            PutF(self, 0x3D0, d);
    } else if (0.0f < d) {
        PutF(self, 0x3D0, d);
    }
    f32 e = port_vu0::Add(Root(Len2(SubL(point, Ld(Q(self, 0x3E0))))), f);
    if (F(self, 0x3D4) < e)
        PutF(self, 0x3D4, e);
}

void UpdateColliderBounds(Collider* self)
{
    const u64 kSphere = u64(1) << 40;     // the part is a sphere: its box is its radius
    const u64 kNoPoint = u64(1) << 44;    // the part has no extra point at +0x370
    L lo = Ld(Q(GLOBAL(0x00248D50))), hi = Ld(Q(GLOBAL(0x00248D60)));
    L pos = Ld(Q(self, 0x140));
    L unit = Ld(Q(GLOBAL(0x00248D40)));
    f32 maxR = 0.0f;
    for (u8* part = Ptr(self, 0x2D8); part; part = Ptr(part, 0x2E4)) {
        u64 flags;
        std::memcpy(&flags, part + 0x538, 8);
        if (flags & kSphere) {
            f32 r = F(part, 0x3CC);
            L c = Ld(Q(part, 0x140));
            MinXyz(lo, Corner(c, unit, -r));
            MaxXyz(hi, Corner(c, unit, r));
        } else {
            if (W(part, 0x3F8) == 0)
                continue;
            s32 n1 = (s32)W(part, 0x3A8), n2 = (s32)W(part, 0x510);
            u8* a1 = Ptr(part, 0x3AC);
            for (s32 i = 0; i < n1; i++) {
                L v = Ld(Q(a1, (u32)i * 16));
                MaxXyz(hi, v);
                MinXyz(lo, v);
            }
            u8* a2 = Ptr(part, 0x514);
            for (s32 i = 0; i < n2; i++) {
                L v = Ld(Q(a2, (u32)i * 16));
                MaxXyz(hi, v);
                MinXyz(lo, v);
            }
        }
        if (!(flags & kNoPoint)) {
            L v = Ld(Q(part, 0x370));
            MaxXyz(hi, v);
            MinXyz(lo, v);
        }
        f32 d = port_vu0::Add(Root(Len2(SubL(Ld(Q(part, 0x140)), pos))), F(part, 0x3CC));
        if (maxR < d)
            maxR = d;
    }
    if (maxR == 0.0f) {
        u128 p = Q(self, 0x140);
        PutF(self, 0x3D0, maxR);
        PutQ(self, 0x420, p);
        PutQ(self, 0x3E0, p);
        PutQ(self, 0x430, p);
        PutF(self, 0x3D4, maxR);
        return;
    }
    PutF(self, 0x3D0, maxR);
    PutQ(self, 0x420, St(lo));
    PutQ(self, 0x430, St(hi));
    L centre = AddL(Scale(lo, 0.5f), Scale(hi, 0.5f));
    PutQ(self, 0x3E0, St(centre));
    f32 radius = 0.0f, best2 = 0.0f;
    for (u8* part = Ptr(self, 0x2D8); part; part = Ptr(part, 0x2E4)) {
        u64 flags;
        std::memcpy(&flags, part + 0x538, 8);
        if (!(flags & kSphere) && W(part, 0x3F8) == 0)
            continue;
        L v = Ld(Q(part, 0x140));
        f32 d = port_vu0::Add(Root(Len2(SubL(v, Ld(Q(self, 0x3E0))))), F(part, 0x3CC));
        if (radius < d) {
            radius = d;
            best2 = Mul(d, d);
        }
        if (flags & kNoPoint)
            continue;
        f32 e2 = Len2(SubL(v, centre));
        if (best2 < e2) {
            best2 = e2;
            radius = Root(e2);   // the FPU's sqrt.s
        }
        f32 e3 = Len2(SubL(Ld(Q(part, 0x370)), centre));
        if (best2 < e3) {
            best2 = e3;
            radius = Root(e3);
        }
    }
    PutF(self, 0x3D4, radius);
}

void TransformMeshToWorld(CollisionMesh* a, CollisionMesh* b, Qword128 q, Mat3* m)
{
    if (W(a, 8) == W(b, 8))
        return;
    L r0 = Ld(Q(m, 0)), r1 = Ld(Q(m, 0x10)), r2 = Ld(Q(m, 0x20));
    L offset = Ld((u128)q);
    for (s32 i = 0; i < (s32)W(a, 4); i++)
        PutQ(Ptr(b, 8), (u32)i * 16, St(AddL(offset, RotateKeepW(r0, r1, r2, Ld(Q(Ptr(a, 8), (u32)i * 16))))));
    for (s32 k = 0; k < (s32)W(a, 0xC); k++) {
        u8* fa = Ptr(a, 0x10) + (u32)k * 32;
        u8* fb = Ptr(b, 0x10) + (u32)k * 32;
        L normal = RotateKeepW(r0, r1, r2, Ld(Q(fa)));
        PutQ(fb, 0, St(normal));
        u16 first;
        std::memcpy(&first, fb + 0x1E, 2);
        PutF(fb, 0x10, Dot3(normal, Ld(Q(Ptr(b, 8), (u32)first * 16))));
        PutQ(Ptr(b, 0x1C), (u32)k * 16, St(AddL(offset, RotateKeepW(r0, r1, r2, Ld(Q(Ptr(a, 0x1C), (u32)k * 16))))));
    }
}

// --- world 5's mode collider and the blend entries

void ComputeWorld5ModeColliderAnchors(World5ModeCollider* self, Vec4* a, Vec4* b)
{
    f32 t = F(self, 0x6FC), u = F(self, 0x700);
    f32 c0 = F(GLOBAL(0x00263C00), 0), c1 = F(GLOBAL(0x00263C00), 4), c2 = F(GLOBAL(0x00263C00), 8);
    f32 d0 = F(GLOBAL(0x00263C10), 0), d1 = F(GLOBAL(0x00263C10), 4), d2 = F(GLOBAL(0x00263C10), 8);
    f32 A = port_vu0::Add(c0, Mul(t, port_vu0::Add(c1, Mul(t, c2))));
    f32 Bv = port_vu0::Add(d0, Mul(u, port_vu0::Add(d1, Mul(u, d2))));
    PutQ(a, 0, Shuffled(0.0f, A, Bv));
    PutQ(b, 0, Shuffled(0.0f, Mul(F(self, 0x704), c1), Mul(F(self, 0x708), d1)));
}

void InterpolateBlendEntryPosition(void* p, void* e, f32 a, f32 b, s32 i, s32 j, void* out)
{
    u8* ei = B(e) + i * 0x50;
    u8* ej = B(e) + j * 0x50;
    f32 rest = port_vu0::Sub(1.0f, b);
    f32 lj = Mul(b, F(ej, 0x34));
    f32 li = Mul(rest, F(ei, 0x34));
    f32 sum = port_vu0::Add(li, lj);
    f32 k = port_vu0::Add(Mul(Mul(a, rest), F(ei, 0x3C)), Mul(Mul(a, b), F(ej, 0x3C)));
    f32 wi = FDiv(li, sum), wj = FDiv(lj, sum);
    L base = Ld(Q(Ptr(p, 0x388), W(p, 0x6D0) * 16));
    L mix;
    for (int n = 0; n < 4; n++)
        mix.v[n] = port_vu0::Add(Mul(Ld(Q(ei, 0x10)).v[n], wi), Mul(Ld(Q(ej, 0x10)).v[n], wj));
    L position = Corner(base, mix, k);
    L target;
    for (int n = 0; n < 4; n++)
        target.v[n] = port_vu0::Add(Mul(Ld(Q(ei, 0x20)).v[n], rest), Mul(Ld(Q(ej, 0x20)).v[n], b));
    L turned = RotateKeepW(Ld(Q(p, 0x660)), Ld(Q(p, 0x670)), Ld(Q(p, 0x680)), SubL(target, position));
    PutQ(out, 0, St(AddL(Ld(Q(Ptr(p, 0x6D4))), turned)));
}

// --- contact values (the object gatherer's and the path follower's, the switched collider's mode)

// Sets the mode word (0x600); entering mode 1 or 3 clears the quadword at 0x5E0 (vsub of vf0).
void SetSwitchedColliderMode(SwitchedCollider* self, s32 mode)
{
    if ((s32)W(self, 0x600) == mode)
        return;
    PutW(self, 0x600, (u32)mode);
    if (mode == 1 || mode == 3)
        PutQ(self, 0x5E0, St(Lanes(0, 0, 0, 0)));
}

// When the contact's normal (0x80) points up (its dot with +z, as the accumulator forms it, over
// 0.7): the contact's two values (0x94, 0x98) become 3.0 (mode word 0x550 is 1) or 5.0, and 0.1.
s32 ComputeObjectGathererContactValues(ObjectGatherer* self, BodyContact* contact)
{
    f32 up = Dot3(Ld(Q(GLOBAL(0x00248D20))), Ld(Q(contact, 0x80)));
    if (FromBits(0x3F333333) < up) {
        PutF(contact, 0x94, W(self, 0x550) == 1 ? 3.0f : 5.0f);
        PutF(contact, 0x98, FromBits(0x3DCCCCCD));
    }
    return 1;
}

// Unless the contact's normal component (0x88), signed for the side (g_contactSideSigns), is under
// 0.7 or the speed (0x61C) is not over 1e-4: the velocity (0x110) scaled by minus the speed, less
// its part along the axis (0x130), is added to the side's record (0x30, 64 bytes a side).
void ResolveObjectGathererContact(ObjectGatherer* self, BodyContact* contact, s32 side)
{
    f32 n = Mul(F(contact, 0x88), F(GLOBAL(0x00274AD0), (u32)side * 4));
    u8* rec = B(contact) + side * 64;
    if (n < FromBits(0x3F333333))
        return;
    f32 speed = F(self, 0x61C);
    if (!(kEpsilon < speed))
        return;
    L axis = Ld(Q(self, 0x130));
    L v = Scale(Ld(Q(self, 0x110)), -speed);
    L along = Scale(axis, Dot3(v, axis));
    PutQ(rec, 0x30, St(AddL(SubL(v, along), Ld(Q(rec, 0x30)))));
}

// The path follower's contact values (0x94, 0x98): 0.15 and 0.5 when the other side's object
// lacks flag bit 56 (0x538); else, when the side's signed normal component is over 0.7 and the
// speed (the length of 0x150) over the threshold at 0x275618, 1.0 and 0.15; else 0 and 0.
void ComputePathFollowerContactValue(PathFollower* self, BodyContact* contacts, s32 side)
{
    u8* other = (u8*)port::Native(W(contacts, (u32)(1 - side) * 64));
    u64 flags;
    std::memcpy(&flags, other + 0x538, 8);
    if (!(flags & (1ull << 56))) {
        PutF(contacts, 0x94, FromBits(0x3E19999A));
        PutF(contacts, 0x98, 0.5f);
        return;
    }
    f32 n = Mul(F(contacts, 0x88), F(GLOBAL(0x00274AD0), (u32)side * 4));
    if (FromBits(0x3F333333) < n) {
        f32 speed = port_vu0::Add(0.0f, Root(Len2(Ld(Q(self, 0x150)))));
        if (F(GLOBAL(0x00275618), 0) < speed) {
            PutF(contacts, 0x94, 1.0f);
            PutF(contacts, 0x98, FromBits(0x3E19999A));
            return;
        }
    }
    PutF(contacts, 0x94, 0.0f);
    PutF(contacts, 0x98, 0.0f);
}

// --- the clock

s64 ReadCycleCounter(void) { return (s64)port::CycleCount(); }

// --- the table by retail address (port/ptr32.h): these functions' pointers are in the game's
// data too (class descriptors), as their retail addresses.

extern "C" const port::NamedFunction port_game_asm_functions[] = {
    PORT_FUNCTION(AddWeightedVectors),
    PORT_FUNCTION(MeasureListenerDistance),
    PORT_FUNCTION(SplitVectorAlongAxis),
    PORT_FUNCTION(BuildPerpendicularPair),
    PORT_FUNCTION(ScaleMatrix),
    PORT_FUNCTION(MultiplyByTransposedMatrix3x3),
    PORT_FUNCTION(OrthonormalizeMatrix),
    PORT_FUNCTION(GetPlacedObjectLocalVelocity),
    PORT_FUNCTION(StorePlacedObjectLocalVelocity),
    PORT_FUNCTION(StorePlacedObjectLocalSpin),
    PORT_FUNCTION(ComputeChainedWorldFrame),
    PORT_FUNCTION(FindPlaneTreeNodeForPoint),
    PORT_FUNCTION(IsPointInsideFaceTree),
    PORT_FUNCTION(LookupValueTreeLeaf),
    PORT_FUNCTION(FindNearestStoredPoint),
    PORT_FUNCTION(FindNearestAgentKey),
    PORT_FUNCTION(CompareDistanceFromPlayer),
    PORT_FUNCTION(MeasurePointGraphEdges),
    PORT_FUNCTION(BuildCameraGroundFrame),
    PORT_FUNCTION(ComputeCameraDistanceFade),
    PORT_FUNCTION(FillLookDirectionBlock),
    PORT_FUNCTION(InitEffect),
    PORT_FUNCTION(InitRangeRecord),
    PORT_FUNCTION(RangeRecordReachesSphere),
    PORT_FUNCTION(ConvertRgbToHsv),
    PORT_FUNCTION(ComputeColliderPointVelocity),
    PORT_FUNCTION(ApplyColliderInverseInertia),
    PORT_FUNCTION(ExpandColliderBoundsBySphere),
    PORT_FUNCTION(UpdateColliderBounds),
    PORT_FUNCTION(TransformMeshToWorld),
    PORT_FUNCTION(ComputeWorld5ModeColliderAnchors),
    PORT_FUNCTION(InterpolateBlendEntryPosition),
    PORT_FUNCTION(ReadCycleCounter),
    PORT_FUNCTION(ScaleQuadword),
    PORT_FUNCTION(IsVelocityNegligible),
    PORT_FUNCTION(IsSpinNegligible),
    PORT_FUNCTION(IsAccelerationNegligible),
    PORT_FUNCTION(IsSpinAccelerationNegligible),
    PORT_FUNCTION(TransformVector4),
    PORT_FUNCTION(AddColliderForce),
    PORT_FUNCTION(AddColliderTorque),
    PORT_FUNCTION(SetSwitchedColliderMode),
    PORT_FUNCTION(ComputeObjectGathererContactValues),
    PORT_FUNCTION(ResolveObjectGathererContact),
    PORT_FUNCTION(ComputePathFollowerContactValue),
    {nullptr, nullptr},
};
