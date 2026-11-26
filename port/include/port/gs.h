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
#include <vector>

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

// A run of PACKED data without A+D: loops times the nreg registers of regs (four bits each), a
// quadword a register, as WritePacked would take them one by one.
void PackedRun(std::uint64_t regs, std::uint32_t nreg, const std::uint32_t* q, std::uint32_t loops);

// IMAGE data: a quadword of the transfer BITBLTBUF, TRXPOS, TRXREG and TRXDIR started.
void Image(const std::uint32_t* q);

// A run of IMAGE data: count quadwords, as Image would take them one by one.
void ImageRun(const std::uint32_t* q, std::uint32_t count);

// A movie's picture given to the renderer as it is (docs/VIDEO.md): the decoder puts each
// picture, numbered, with DirectPicturePut (any thread); the game's packet then writes the
// number and the texture's place to register 0x7F, which no GS has, and from there on a draw
// with that texture is given the picture (DirectPicture, for the renderer) until anything is
// uploaded to the GS again. HasBackend says whether there is a renderer to take one.
void DirectPicturePut(std::uint32_t number, const std::uint32_t* rgba, int width, int height);
// The same without a copy: the picture's memory is taken, and rgba left with memory to use again.
void DirectPictureTake(std::uint32_t number, std::vector<std::uint32_t>& rgba, int width, int height);
struct DrawState;
bool DirectPicture(const DrawState& state, const std::uint32_t** rgba, int* width, int* height, std::uint32_t* serial);
bool HasBackend();

// The display (the game's double buffer, sceGsSwapDBuff's): DISPFB and DISPLAY's values.
void SetDisplay(std::uint64_t dispfb, std::uint64_t display);

// The displayed frame as 8-bit RGB, width by height (the display's size); false before any.
bool ReadDisplay(std::string& rgb, int& width, int& height);

// An 8-bit RGB picture written as a PNG (the frame dump's writer, for a renderer's own pictures).
void WritePngFile(const std::string& path, const std::string& rgb, int width, int height);

// A frame was shown (the game swapped its double buffer): written to the dump folder when due.
void FrameShown();

// --dump-fb: every every-th frame shown written to dir as frame-NNNNN.png.
void SetFrameDump(const std::string& dir, int every);

// The frames shown so far (the game's buffer swaps).
std::uint64_t FramesShown();

// SLY1_GS_SURVEY: the drawing states the game uses, counted by primitive: their report.
void SurveyReport();

// The uploads so far: those whose data came in one run, how many of them were not written
// again (the same data to the same place with nothing uploaded there since), and the others.
void UploadCounts(std::uint64_t* whole, std::uint64_t* skipped, std::uint64_t* other);
// The uploads whose pages were put back as kept (ImageRun), and with SLY1_UPLOAD_VERIFY=1 how many
// of them were not what writing them gave.
void UploadsRestored(std::uint64_t* restored, std::uint64_t* wrong);

// The primitives drawn so far.
std::uint64_t PrimitivesDrawn();

}  // namespace gs
}  // namespace port

#endif
