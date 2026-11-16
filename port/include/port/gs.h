// SPDX-License-Identifier: AGPL-3.0-or-later
// The GS (port/render/gs.cpp): its registers, its local memory and what it draws, fed by the GIF's
// three paths (port/platform/dmac.cpp: VU1's XGKICK, VIF1's DIRECT, the GIF channel). Written from
// the public descriptions of the GS's registers, primitives and pixel pipeline. The host draws
// into local memory as the GS does (port/render/gs_raster.cpp), the reference the Switch's GPU
// renderer is checked against.
#ifndef PORT_GS_H
#define PORT_GS_H

#include <cstdint>
#include <string>

namespace port {
namespace gs {

// Holds the GS for a run of writes (a whole GIF packet, a render queue item). Write, WritePacked
// and Image are called with it held; the display's functions take it themselves.
class Hold {
public:
    Hold();
    ~Hold();
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
};

// A register write: the register's address and its 64 bits (REGLIST, A+D, or a PACKED write
// in register form).
void Write(std::uint32_t reg, std::uint64_t value);

// A PACKED quadword for register reg (0 to 0xD: PRIM, RGBAQ, ST, UV, XYZF2, XYZ2, TEX0, CLAMP,
// FOG, XYZF3, XYZ3), in its packed layout.
void WritePacked(std::uint32_t reg, const std::uint32_t* q);

// IMAGE data: a quadword of the transfer BITBLTBUF, TRXPOS, TRXREG and TRXDIR started.
void Image(const std::uint32_t* q);

// The display (the game's double buffer, sceGsSwapDBuff's): DISPFB and DISPLAY's values.
void SetDisplay(std::uint64_t dispfb, std::uint64_t display);

// The displayed frame as 8-bit RGB, width by height (the display's size); false before any.
bool ReadDisplay(std::string& rgb, int& width, int& height);

// A frame was shown (the game swapped its double buffer): written to the dump folder when due.
void FrameShown();

// --dump-fb: every every-th frame shown written to dir as frame-NNNNN.png.
void SetFrameDump(const std::string& dir, int every);

// The frames shown so far (the game's buffer swaps).
std::uint64_t FramesShown();

// The primitives drawn so far.
std::uint64_t PrimitivesDrawn();

}  // namespace gs
}  // namespace port

#endif
