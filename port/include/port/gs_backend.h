// SPDX-License-Identifier: AGPL-3.0-or-later
// What the GS hands a renderer that draws with a GPU (port/switch/gpu.cpp) in place of the
// host's reference drawing into local memory: runs of primitives with the registers they are
// drawn by, the textures in local memory decoded, and the display's swaps.
#ifndef PORT_GS_BACKEND_H
#define PORT_GS_BACKEND_H

#include <cstddef>
#include <cstdint>
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
    // The game swapped its double buffer: DISPFB's buffer is the one to show.
    virtual void Present(std::uint64_t dispfb, std::uint64_t display) = 0;
};

// The renderer the GS draws through, or none (the reference drawing, when --dump-fb asks).
void SetBackend(Backend* backend);

// For a backend, inside Begin. The state's texture as 32-bit RGBA (the GS's alpha, 0x80 for 1),
// tw by th, through the palette loaded now.
void DecodeTexture(const DrawState& state, std::vector<std::uint32_t>& rgba, int& width, int& height);

// A number that changes when an upload may have changed the state's texture or its palette.
std::uint32_t TextureGeneration(const DrawState& state);

// For a backend, inside Begin. A hash of what the state's texture is read from: the pages of
// local memory it lies in and the palette loaded.
std::uint64_t TextureSource(const DrawState& state);

}  // namespace gs
}  // namespace port

#endif
