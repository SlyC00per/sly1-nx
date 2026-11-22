// SPDX-License-Identifier: AGPL-3.0-or-later
// What the GS hands a renderer that draws with a GPU (port/switch/gpu.cpp) in place of the
// host's reference drawing into local memory: runs of primitives with the registers they are
// drawn by, the textures in local memory decoded, and the display's swaps.
#ifndef PORT_GS_BACKEND_H
#define PORT_GS_BACKEND_H

#include <cstddef>
#include <cstdint>
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
};
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
};

// The renderer the GS draws through, or none (the reference drawing, when --dump-fb asks).
void SetBackend(Backend* backend);

// A batch after a GIF tag (its PRE and PRIM as tagWord1 has them): given to the renderer as the
// tag's vertices would have been, in order with everything else. False, with nothing drawn, when
// there is no renderer that takes it or the primitive is not a smooth-shaded strip with S, T, Q.
bool DrawBatch(std::uint32_t tagWord1, Batch& batch);

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
std::uint32_t BufferGeneration(std::uint32_t fbp, std::uint32_t fbw, std::uint32_t width, std::uint32_t height);

// For a backend, inside Begin. A hash of what the state's texture is read from: the pages of
// local memory it lies in and the palette loaded.
std::uint64_t TextureSource(const DrawState& state);

}  // namespace gs
}  // namespace port

#endif
