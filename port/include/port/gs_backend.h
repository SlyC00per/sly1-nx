// SPDX-License-Identifier: AGPL-3.0-or-later
// What the GS hands a renderer that draws with a GPU (port/switch/gpu.cpp) in place of the
// host's reference drawing into local memory: runs of primitives with the registers they are
// drawn by, the textures in local memory decoded, and the display's swaps.
#ifndef PORT_GS_BACKEND_H
#define PORT_GS_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>

namespace port {
namespace gs {

// A vertex as the GS takes it. x and y are pixels of the frame buffer (XYOFFSET taken off, the
// odd field's half line dropped: the fields are drawn as whole pictures); s, t and q as the
// primitive gave them (UV coordinates as s and t over the texture's size, q 1).
struct DrawVertex {
    float x, y;
    std::uint32_t z;
    std::uint8_t r, g, b, a;
    float s, t, q;
    float fog;   // 0 to 255
};

// The registers a run of primitives is drawn by (the context's the primitive chose).
struct DrawState {
    std::uint32_t kind;   // 0 points, 1 lines, 2 triangles (strips, fans and sprites as triangles)
    std::uint32_t tme, fge, abe, fba;
    std::uint64_t tex0, tex1, clamp, alpha, test, frame, zbuf, scissor, texa, fogcol;
};

// A batch of the game's vertices as VU1 was given them (port/render/vu1_handlers.cpp), for a
// renderer that transforms them itself in place of VU1's program: a triangle strip of count
// vertices, each stride quadwords of VU1's data memory, with what the program transforms them by.
// The renderer does what the program does to each vertex (the matrix, the divide by w, the
// viewport's scale and offset, the fog from w) and what the program and the clipper after it do
// to each triangle (those outside the view cut or dropped, a backface dropped when cull is set
// and the triangle's last vertex has kCullable in its control word).
// A lit batch's colours as the game's programs make them, for a renderer to make itself
// (docs/RENDERER.md, "A lit part's colours left to the renderer"). The programs light a batch by
// up to four lights, each a direction in the batch's own space with a base, a scale and a colour
// (0x0FE of chunk E1624411: a directional light, or a point light taken as one from the object's
// centre), and then make three colours of each vertex (0x242 of chunk B90790F3), one for each
// time the batch is drawn: the surface and its two lights' shares.
struct LitLight {
    float dir[3], base[3], scale[3], colour[3];
};
struct LitParams {
    std::uint32_t lights = 0;
    LitLight light[4];
    float start[2];        // what each vertex's two sums start from
    float one[3];          // vf01's x, y and z
    float colourScale;     // the header's 10.w, which the kicks' colours are scaled by
};
// What a lit batch carries of its lighting (Batch::lit), as words: the parameters it was lit with,
// what its mesh adds to the second sum's start (13.y / 32768), the count of its vertices, then
// each vertex's unit normal (three words) and each one's base colour and alpha (four).
constexpr std::size_t kLitParamWords = (sizeof(LitParams) + 3) / 4;
constexpr std::size_t kLitHead = kLitParamWords + 2;
inline std::size_t LitWords(std::uint32_t count) { return kLitHead + (std::size_t)count * 7; }
// The three colours (R, G, B each) of a vertex with this unit normal and base colour (R, G, B as
// the mesh has them, 0 to 255): [0] the surface's, [1] and [2] the shares'. The programs'
// arithmetic in the host's floats: within one of what they give.
inline void LitColoursOf(const LitParams& p, const float normal[3], const std::uint32_t base[3], std::int32_t out[3][3])
{
    float sx = p.start[0], sy = p.start[1], lit[3] = {0.0f, 0.0f, 0.0f};
    for (std::uint32_t k = 0; k < p.lights; k++) {
        const LitLight& l = p.light[k];
        const float c = normal[0] * l.dir[0] + normal[1] * l.dir[1] + normal[2] * l.dir[2];
        const float v = c + c * c * c;
        float a[3];
        for (int i = 0; i < 3; i++) {
            a[i] = l.base[i] + l.scale[i] * v;
            if (!(a[i] > 0.0f))
                a[i] = 0.0f;
        }
        sx += a[0];
        sy += a[1];
        for (int i = 0; i < 3; i++)
            lit[i] += l.colour[i] * a[2];
    }
    float m = lit[0] > lit[1] ? lit[0] : lit[1];
    m = m > lit[2] ? m : lit[2];
    float tx = p.one[0] - m - p.one[0] * sy, ty = p.one[1] - m;
    tx = sx < tx ? sx : tx;
    ty = sy < ty ? sy : ty;
    tx = tx > 0.0f ? tx : 0.0f;
    ty = ty > 0.0f ? ty : 0.0f;
    const float third = 0.33333298563957214f;   // (0x3EAAAA9F, the program's constant)
    const float b[3] = {(float)(std::int32_t)base[0], (float)(std::int32_t)base[1], (float)(std::int32_t)base[2]};
    const float a = third * b[0] + third * b[1] + third * b[2];
    const float s = p.colourScale;
    for (int i = 0; i < 3; i++) {
        const float capped = lit[i] < p.one[i] ? lit[i] : p.one[i];
        const std::int32_t c0 = (std::int32_t)(a * tx), c1 = (std::int32_t)(b[i] * ty), c2 = (std::int32_t)(capped * a);
        out[0][i] = (std::int32_t)((float)c0 * s);
        out[1][i] = (std::int32_t)((float)c1 * s);
        out[2][i] = (std::int32_t)((float)c2 * s);
    }
}

struct Batch {
    const std::uint32_t* quads;   // count * stride quadwords
    std::uint32_t count, stride;
    std::uint32_t positionAt, stqAt, colourAt;   // quadwords of a vertex: x y z and the control word; S T Q; R G B A
    float matrix[16];             // four columns
    float scale[4], offset[4];    // of x, y, z over w, to the GS's coordinates times 1,024 (z times 16,384), past 2 to the 33rd
    float fogScale, wMax;         // fog: the control word's low 12 bits plus fogScale times min(w, wMax) / w, over 16
    bool cull;
    int ofx, ofy;                 // XYOFFSET (12.4), the odd field's half line dropped
    std::uint32_t id = 0;         // the same for the kicks of one batch (their positions and STQ the
                                  // same, the colours not): a renderer may keep what it took of the first
    std::uint64_t keep = 0;       // not 0: the same for as long as the batch's vertices and colours are
                                  // (a model part drawn again, port/platform/dmac.cpp): a renderer may keep
                                  // them in memory of its own from frame to frame, until ForgetKept
    // What the game's programs do to every vertex of a part by one number of the part's own, left
    // for the renderer to do, so that a part kept from frame to frame is drawn with this frame's
    // numbers (docs/RENDERER.md, "A part's numbers left to the renderer"):
    float alphaScale = 1.0f;      // each vertex's alpha times this, truncated (the header's 9.x, a fade)
    float stOffset[2] = {0.0f, 0.0f};   // added to each vertex's S and T (a texture that slides: 22.x and y)
    bool alphaDeferred = false;   // alphaScale is the header's, not yet in the colours
    bool stApplied = false;       // the program that adds 22.x and y ran on the batch (they are in its S and T)
    std::uint8_t stSource = 0xFF; // while a part is kept: which of its packet's unpacks to 22 the batch's is
    // A lit batch's lighting (kLitHead and after), when the programs' lights were all of the kind
    // LitColoursOf follows: litWords words after the batch's vertices where a batch is kept as
    // words. litKick: which of the three colours this kick draws. litNow, set for a part drawn
    // again: the lighting's parameters this frame, for the renderer to make the kick's colours
    // from (the vertices' own are those of the frame the part was kept in).
    const std::uint32_t* lit = nullptr;
    std::uint32_t litWords = 0;
    std::uint8_t litKick = 0;
    const LitParams* litNow = nullptr;
    // A batch whose vertices a sway moved before its transform (0x750, 0x752, 0x754 of chunk
    // 51A89CD8: each vertex's position plus its four weights times four displacements, rows 22 to
    // 25 turned into the part's space): sway is swayWords words after its lighting's (kSwayHead
    // and after: the four displacements it was moved by, then each vertex's weights). swaySource:
    // which of its packet's unpacks of those rows it was. swayMoved, set for a part drawn again:
    // quads are its vertices moved by this frame's displacements (SwayQuads), not those kept.
    const std::uint32_t* sway = nullptr;
    std::uint32_t swayWords = 0;
    std::uint8_t swaySource = 0xFF;
    bool swayMoved = false;
    // A skinned batch (0x401 and 0x411 of chunk 2DDC9AA2: each vertex's position through four
    // matrices blended by its weights, its normal through their 3x3 parts, made unit length):
    // skin is skinWords words after its sway's (kSkinHead and after: where its four matrices were
    // in VU1's memory, from its TOP, and whether its normals were skinned; then each vertex's
    // position and normal as they were before, and its weights). boneSource: for each of the four
    // matrices, which of its packet's bone unpacks it was (0xFF: none known). skinMoved, set for a
    // part drawn again: quads are its vertices skinned by this frame's bones (SkinQuads).
    const std::uint32_t* skin = nullptr;
    std::uint32_t skinWords = 0;
    std::uint8_t boneSource[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    bool skinMoved = false;
    // A lit part drawn again whose colours for this kick were made already (count * 4 words, as
    // LitColoursNow makes them: the helper's, port/platform/dmac.cpp's PrepTask): drawn as they are.
    const std::uint32_t* coloursNow = nullptr;
    // A batch drawn again by 0x076 of chunk E1624411 (a texture laid on a mesh from another view: the
    // shadow under a character): the same vertices, each one's S, T and Q from its clip position
    // through proj (four columns), Q kept within projLimits' x and y (the S and T moved by half the
    // change, the alpha 0 below projLimits' w when it was), the colour projColour (RGBA words).
    // projSource: which of its packet's unpacks to 22 proj was made of (as swaySource).
    bool projected = false;
    float proj[16] = {};
    float projLimits[4] = {};
    std::uint32_t projColour[4] = {};
    std::uint8_t projSource = 0xFF;
};
// A projected batch's vertex: its S, T and Q (undivided, as a batch's STQ) and its colour, from its
// clip position (0x076's arithmetic).
inline void ProjectedOf(const Batch& b, const float clip[4], float stq[3], std::uint32_t colour[4])
{
    float c[4];
    for (int i = 0; i < 4; i++)
        c[i] = b.proj[i] * clip[0] + b.proj[4 + i] * clip[1] + b.proj[8 + i] * clip[2] + b.proj[12 + i] * clip[3];
    float q = c[3], s = c[0], t = c[1];
    colour[0] = b.projColour[0];
    colour[1] = b.projColour[1];
    colour[2] = b.projColour[2];
    colour[3] = b.projColour[3];
    const float below = c[3] - b.projLimits[0], above = c[3] - b.projLimits[1];
    const bool small = below < 0.0f;
    if (small || !(above < 0.0f)) {
        const float limit = small ? b.projLimits[0] : b.projLimits[1];
        const float shift = (limit - c[3]) * 0.5f;
        q = limit;
        s += shift;
        t += shift;
        if (c[3] - b.projLimits[3] < 0.0f)
            colour[3] = 0;
    }
    stq[0] = s;
    stq[1] = t;
    stq[2] = q;
}
constexpr std::size_t kSkinHead = 2 + 64;   // (and the four matrices it was skinned by, 2 to 65)
inline std::size_t SkinWords(std::uint32_t count) { return kSkinHead + (std::size_t)count * 10; }
// Whether any vertex of a skinned batch weighs its matrix j (a mesh of fewer than four bones
// leaves the other places as they were, weighed 0 by every vertex).
inline bool SkinUses(const std::uint32_t* skin, std::uint32_t count, int j)
{
    const float* w = reinterpret_cast<const float*>(skin + kSkinHead + (std::size_t)count * 6);
    for (std::uint32_t k = 0; k < count; k++)
        if (w[k * 4 + j] != 0.0f)
            return true;
    return false;
}
// A skinned batch's vertices by this frame's four matrices (64 floats, as VU1 has them: four
// quadwords each, the translation last): out is the batch's quadwords with each position skinned
// anew; normals (count * 3) each normal skinned anew and made unit length, when the batch's
// normals were skinned (skin[1]) and normals is given. The host's arithmetic: within a rounding.
inline void SkinQuads(const Batch& b, const float* mats, std::uint32_t* out, float* normals)
{
    const float* pos = reinterpret_cast<const float*>(b.skin + kSkinHead);
    const float* nrm = pos + (std::size_t)b.count * 3;
    const float* w = nrm + (std::size_t)b.count * 3;
    const std::size_t words = (std::size_t)b.count * b.stride * 4;
    std::memcpy(out, b.quads, words * 4);
    for (std::uint32_t k = 0; k < b.count; k++) {
        const float* p = pos + k * 3, * n = nrm + k * 3, * wk = w + k * 4;
        float sp[3] = {0, 0, 0}, sn[3] = {0, 0, 0};
        for (int j = 0; j < 4; j++) {
            const float* m = mats + j * 16;
            for (int c = 0; c < 3; c++) {
                sp[c] += wk[j] * (m[c] * p[0] + m[4 + c] * p[1] + m[8 + c] * p[2] + m[12 + c]);
                sn[c] += wk[j] * (m[c] * n[0] + m[4 + c] * n[1] + m[8 + c] * n[2]);
            }
        }
        float* q = reinterpret_cast<float*>(out + (k * b.stride + b.positionAt) * 4);
        q[0] = sp[0];
        q[1] = sp[1];
        q[2] = sp[2];
        if (normals && b.skin[1]) {
            const float len2 = sn[0] * sn[0] + sn[1] * sn[1] + sn[2] * sn[2];
            const float over = len2 == 0.0f ? 3.4028235e38f : 1.0f / std::sqrt(len2);
            for (int c = 0; c < 3; c++)
                normals[k * 3 + c] = sn[c] * over;
        }
    }
}
constexpr std::size_t kSwayHead = 12;   // four displacements, x y z each
inline std::size_t SwayWords(std::uint32_t count) { return kSwayHead + (std::size_t)count * 4; }
// A swayed batch's vertices moved by now's displacements (12 floats) in place of those it was
// moved by: out is the batch's count * stride quadwords with each position moved by the change.
inline void SwayQuads(const Batch& b, const float* now, std::uint32_t* out)
{
    const float* was = reinterpret_cast<const float*>(b.sway);
    const float* weights = was + kSwayHead;
    float d[4][3];
    for (int i = 0; i < 4; i++)
        for (int c = 0; c < 3; c++)
            d[i][c] = now[i * 3 + c] - was[i * 3 + c];
    const std::size_t words = (std::size_t)b.count * b.stride * 4;
    std::memcpy(out, b.quads, words * 4);
    for (std::uint32_t k = 0; k < b.count; k++) {
        float* p = reinterpret_cast<float*>(out + (k * b.stride + b.positionAt) * 4);
        const float* w = weights + k * 4;
        for (int c = 0; c < 3; c++)
            p[c] += w[0] * d[0][c] + w[1] * d[1][c] + w[2] * d[2][c] + w[3] * d[3][c];
    }
}
// The three kicks' colours at once (out: three runs of count * 4 words): a batch's kicks are drawn
// one after another, and the formula gives all three for the cost of one.
inline void LitColoursAll(const Batch& b, std::uint32_t* out)
{
    const float* normal = reinterpret_cast<const float*>(b.lit + kLitHead);
    const std::uint32_t* base = b.lit + kLitHead + (std::size_t)b.count * 3;
    for (std::uint32_t k = 0; k < b.count; k++) {
        std::int32_t c[3][3];
        LitColoursOf(*b.litNow, normal + k * 3, base + k * 4, c);
        for (int kick = 0; kick < 3; kick++) {
            std::uint32_t* o = out + ((std::size_t)kick * b.count + k) * 4;
            o[0] = (std::uint32_t)c[kick][0];
            o[1] = (std::uint32_t)c[kick][1];
            o[2] = (std::uint32_t)c[kick][2];
            o[3] = base[k * 4 + 3];
        }
    }
}
// A lit batch's colours for this frame (Batch::litNow), count times R, G, B and the alpha, as a
// kick's vertices have them.
inline void LitColoursNow(const Batch& b, std::uint32_t* out)
{
    const float* normal = reinterpret_cast<const float*>(b.lit + kLitHead);
    const std::uint32_t* base = b.lit + kLitHead + (std::size_t)b.count * 3;
    for (std::uint32_t k = 0; k < b.count; k++) {
        std::int32_t c[3][3];
        LitColoursOf(*b.litNow, normal + k * 3, base + k * 4, c);
        out[k * 4] = (std::uint32_t)c[b.litKick][0];
        out[k * 4 + 1] = (std::uint32_t)c[b.litKick][1];
        out[k * 4 + 2] = (std::uint32_t)c[b.litKick][2];
        out[k * 4 + 3] = base[k * 4 + 3];
    }
}
constexpr std::uint32_t kStripStart = 0x8000, kCullable = 0x4000;   // bits of a vertex's control word

// What a renderer's vertex shader computes of a batch's vertex k (port/switch/shaders/
// batch_vsh.glsl is this arithmetic): its clip coordinates, and from them the GS's x and y (12.4,
// before XYOFFSET), depth (24 bits) and fog. SLY1_GPU_BATCH_CHECK compares these with what VU1's
// program leaves (port/render/vu1_handlers.cpp).
struct BatchVertex {
    float clip[4];
    float x, y, z, fog;
};
inline BatchVertex VertexOf(const Batch& b, std::uint32_t k)
{
    const std::uint32_t* q = b.quads + (k * b.stride + b.positionAt) * 4;
    float p[3];
    std::memcpy(p, q, 12);
    BatchVertex v;
    for (int c = 0; c < 4; c++)
        v.clip[c] = b.matrix[c] * p[0] + b.matrix[4 + c] * p[1] + b.matrix[8 + c] * p[2] + b.matrix[12 + c];
    const float base = 8589934592.0f;   // 2 to the 33rd
    float w = v.clip[3], over = w != 0.0f ? 1.0f / w : 0.0f;
    v.x = v.clip[0] * over * (b.scale[0] / 1024.0f) + (b.offset[0] - base) / 1024.0f;
    v.y = v.clip[1] * over * (b.scale[1] / 1024.0f) + (b.offset[1] - base) / 1024.0f;
    v.z = v.clip[2] * over * (b.scale[2] / 16384.0f) + (b.offset[2] - base) / 16384.0f;
    float near = w < b.wMax ? w : b.wMax;
    v.fog = (float)(((int)((float)(q[3] & 0xFFF) + b.fogScale * near * over) >> 4) & 0xFF);
    return v;
}

class Backend {
public:
    virtual ~Backend() {}
    // A run starts: the registers its primitives will be drawn by. Called with the GS held, while
    // the palette the state's texture is read through is the one loaded (DecodeTexture).
    virtual void Begin(const DrawState& state) = 0;
    // The run begun: its vertices, and its primitives as indices of them (kind's, one after
    // another). Called with the GS held.
    virtual void Draw(const DrawState& state, const DrawVertex* v, std::size_t vertices, const std::uint32_t* index,
                      std::size_t indices) = 0;
    // A batch for the renderer to transform, drawn by state after everything handed over before;
    // false when the renderer does not take batches (the caller has VU1's program do it).
    virtual bool TakesBatches() const { return false; }
    virtual bool DrawBatch(const DrawState&, const Batch&) { return false; }
    // The batches with keep from first on, count of them, will not come again.
    virtual void ForgetKept(std::uint64_t, std::uint64_t) {}
    // A batch with this keep comes soon: what the renderer will look up for it may be asked of
    // memory now (called on the thread that draws, without the GS held).
    virtual void KeptAhead(std::uint64_t) {}
    // The game swapped its double buffer: DISPFB's buffer is the one to show.
    virtual void Present(std::uint64_t dispfb, std::uint64_t display) = 0;
    // A kept model part drawn again (port/platform/dmac.cpp, gs::PartBegin): the renderer may keep
    // the commands its batches give, and give them again in a later frame without the batches
    // drawn one by one. PartBegin's answer: 0 the part is drawn as ever; 1 drawn as ever and its
    // commands kept (PartBatch says which of the part's batches comes next to DrawBatch); 2 kept
    // commands given again: no batch comes to DrawBatch, each comes to PartParams with this frame's
    // numbers instead. PartEnd after the part's last batch; clean is false when anything but its
    // batches was drawn in the meantime (the commands kept are then not to be used).
    virtual bool TakesParts() const { return false; }
    // (flags: 1 the part's alpha's scale changes from frame to frame: its alpha tests are not to be
    // decided by its vertices' alpha, so that its commands do not depend on it)
    virtual int PartBegin(const void*, std::uint64_t, std::uint32_t, std::uint32_t) { return 0; }
    virtual void PartBatch(std::uint32_t) {}
    virtual void PartParams(std::uint32_t, const Batch&) {}
    virtual void PartEnd(bool) {}
};

// The renderer the GS draws through, or none (the reference drawing, when --dump-fb asks).
void SetBackend(Backend* backend);

// A batch after a GIF tag (its PRE and PRIM as tagWord1 has them): given to the renderer as the
// tag's vertices would have been, in order with everything else. False, with nothing drawn, when
// there is no renderer that takes it or the primitive is not a smooth-shaded strip with S, T, Q.
bool DrawBatch(std::uint32_t tagWord1, Batch& batch);

// A kept model part drawn again, through the renderer's part calls (Backend::PartBegin): the
// GS's run before it drawn, and the renderer told the part, its key and how many batches it has;
// the key is joined with the registers' (but for what goes with the vertices, the transfers and
// XYOFFSET, which the batches take as numbers of their own). PartReplayBatch, when PartBegin gave
// 2: a batch's tag taken as DrawBatch takes it (PRIM), its XYOFFSET put in, and the batch given to
// the renderer's PartParams; false where DrawBatch would draw nothing. PartEnd tells the renderer
// whether the GS drew anything else meanwhile or had anything uploaded.
bool PartsTaken();   // whether the renderer takes kept parts at all now (PartBegin is worth asking)
int PartBegin(const void* part, std::uint64_t sig, std::uint64_t key, std::uint32_t batches, std::uint32_t flags);
void PartBatch(std::uint32_t index);
bool PartReplayBatch(std::uint32_t tagWord1, Batch& batch, std::uint32_t index);
void PartEnd();

// A batch from VU1's handlers: DrawBatch at once, or handed to whoever SetBatchSink named (the GS
// thread's queue, port/platform/dmac.cpp: the batch is then drawn later, in order with the GS's
// packets, and counts as taken).
bool KickBatch(std::uint32_t tagWord1, Batch& batch);
// A model part drawn from its light cache's packet (the hook of tools/patches.py, seen by
// port/platform/dmac.cpp): open from the hook to the return from the packet's call. The batches
// kicked meanwhile are the part's. PartCounts: the batches and vertices kicked inside parts and
// outside them, and the parts, so far.
void PartOpen(bool open, std::uint32_t packet, std::uint32_t half);
void PartCounts(std::uint64_t in[2], std::uint64_t out[2], std::uint64_t* parts);
// Batch::keep's keys from first on, count of them, will not come again (the backend may let go of
// what it kept for them).
void ForgetKept(std::uint64_t first, std::uint64_t count);
// A batch with this keep is about to be handed over (Backend::KeptAhead).
void KeptAhead(std::uint64_t keep);
void SetBatchSink(bool (*sink)(std::uint32_t tagWord1, const Batch& batch));

// Whether the renderer set takes batches.
bool TakesBatches();

// SLY1_GPU_CAPTURE's views: how many of a frame's first batches the renderer asks the program to
// transform after all (a diagnosis of the renderer's own drawing of them).
void SetFirstBatchesKept(int count);
int FirstBatchesKept();

// For a backend, inside Begin. The state's texture as 32-bit RGBA (the GS's alpha, 0x80 for 1),
// tw by th, through the palette loaded now.
void DecodeTexture(const DrawState& state, std::vector<std::uint32_t>& rgba, int& width, int& height);

// A number that changes when an upload may have changed the state's texture or its palette.
std::uint32_t TextureGeneration(const DrawState& state);

// The uploads so far, and the latest of them (as that count was when it started) into the pages
// of a 32-bit buffer at frame base fbp, fbw * 64 wide: a backend that keeps such a buffer as an
// image of its own (the game drew into it) learns so that the game has since put textures there.
std::uint32_t UploadSerial();
// (SLY1_PERF_LOG) The quadwords of image data the GS has taken so far; palettes read from its
// memory [0] and copied from the cache of them [1].
std::uint64_t UploadedQuadwords(std::uint64_t palettes[2]);
std::uint32_t BufferGeneration(std::uint32_t fbp, std::uint32_t fbw, std::uint32_t width, std::uint32_t height);

// For a backend, inside Begin. A hash of what the state's texture is read from: the pages of
// local memory it lies in and the palette loaded.
std::uint64_t TextureSource(const DrawState& state);

}  // namespace gs
}  // namespace port

#endif
