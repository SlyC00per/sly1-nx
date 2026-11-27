// SPDX-License-Identifier: AGPL-3.0-or-later
// The platform layer: the disc, pad, GS and DMA libraries the game calls (sly1-decomp src/lib's
// cdvd.c, pad.c, graph.c and dma.c; their roles in config/names/lib).
//
// Disc: a read is done when it is started, from the player's image (port/iso.h); every status
//   call reports a ready drive with no error.
// Pad: port/platform/pad.cpp.
// GS and DMA: the port's renderer takes the game's packets at the DMA calls (milestone 4); until
//   then they are accepted and dropped. A DMA channel the game asks for is a block of memory of
//   the channel's register layout inside game memory, so the game's writes to it land somewhere
//   harmless and its reads find a channel that is never busy.
#include <cstdio>
#include <cstring>

#include "common.h"
#include "port/dmac.h"
#include "port/gs.h"
#include "port/iso.h"
#include "port/ptr32.h"

namespace {

std::uint32_t g_discReadCount;

}  // namespace

extern "C" {

// --- the disc

s32 InitDiscLibrary(s32) { return 1; }
s32 WaitDiscReady(s32) { return 2; }                 // ready
s32 SyncDiscNCommand(s32) { return 0; }              // the command has ended / none is running
u32 SendDiscSCommand22(u32) { return 1; }
// The disc's kind as the drive reports it: 0x14, a PS2 DVD, the value the game waits for
// (RefreshDiscStatus; LoadIopModules hangs on purpose without it, as a PS2 with no disc does).
s32 GetDiscStatus() { return 0x14; }
s32 GetDiscError() { return 0; }
s32 StopDiscRead() { return 1; }
s32 GetDiscReadPosition() { return (s32)g_discReadCount; }

s32 CheckDiscChanged(s32, s32* out)
{
    if (out)
        *out = 0;
    return 1;
}

// Reads count sectors from lsn into buffer; the read is done when the call returns.
s32 ReadDiscSectors(s32 lsn, s32 sectors, void* buffer, void*)
{
    if (!port::ReadSectors((std::uint32_t)lsn, (std::uint32_t)sectors, buffer)) {
        std::fprintf(stderr, "port: disc read of %d sectors at %d failed\n", (int)sectors, (int)lsn);
        return 0;
    }
    g_discReadCount += (std::uint32_t)sectors;
    return 1;
}

// --- the GS: the graphics library (sly1-decomp src/lib/graph.c), its logic as the decompilation
// has it; its register writes land in the GS's register window (port/ptr32.h), its packets go
// through the port's DMA (SendGifNormal), and the display it sets is the GS model's (port/gs.h).

namespace {

// GsParam: interlace, video mode, field mode, GS version (the second register layout: not 1).
struct {
    s16 interlace = 1, mode = 2, field = 1, version = 3;
} g_gsParams;

void WriteGsRegister(std::uint32_t addr, std::uint64_t v) { *(volatile std::uint64_t*)port::Native(addr) = v; }

s32 CountFrameBufferPages(s16 psm, s16 w, s16 h)
{
    s32 yS = (w + 63) / 64;
    s32 c = (psm & 2) ? (h + 63) / 64 : (h + 31) / 32;
    if (g_gsParams.interlace == 1 && g_gsParams.field == 0)
        return (s16)(yS * c);
    return (s16)(yS * c * 2);
}

#define GS_DISPLAY(dx, dy, magh, dw, dh) \
    (((std::uint64_t)(magh) << 23) | ((std::uint64_t)(dh) << 44) | (std::uint64_t)(dx) | ((std::uint64_t)(dy) << 12) | \
     ((std::uint64_t)(dw) << 32))

// A display environment (PMODE, SMODE2, DISPFB, DISPLAY, BGCOLOR) for a format and size.
void DefineDisplayEnvironment(std::uint64_t* env, s16 psm, s16 w, s16 h, s16 dx, s16 dy)
{
    env[0] = 0x66;
    env[1] = g_gsParams.interlace != 0 ? (g_gsParams.field != 0 ? 3 : 1) : 2;
    env[2] = ((std::uint64_t)(psm & 0xF) << 15) | ((std::uint64_t)(((w + 63) >> 6) & 0x3F) << 9);
    s32 magh = (w + 0x9FF) / w;
    if (g_gsParams.mode == 2) {
        if (g_gsParams.interlace == 1)
            env[3] = GS_DISPLAY(((std::uint64_t)(dx * magh) + 0x27C) & 0xFFF, (dy + 0x32) & 0xFFF, magh - 1, magh * w - 1,
                                g_gsParams.field != 0 ? h * 2 - 1 : h - 1);
        else
            env[3] = GS_DISPLAY(((std::uint64_t)(dx * magh) + 0x27C) & 0xFFF, (dy + 0x19) & 0xFFF, magh - 1, magh * w - 1, h - 1);
    } else {
        if (g_gsParams.interlace == 1)
            env[3] = GS_DISPLAY(((std::uint64_t)(dx * magh) + 0x290) & 0xFFF, (dy + 0x48) & 0xFFF, magh - 1, magh * w - 1,
                                g_gsParams.field != 0 ? h * 2 - 1 : h - 1);
        else
            env[3] = GS_DISPLAY(((std::uint64_t)(dx * magh) + 0x290) & 0xFFF, (dy + 0x24) & 0xFFF, magh - 1, magh * w - 1, h - 1);
    }
    env[4] = 0;
}

// A drawing environment: FRAME_1, ZBUF_1, XYOFFSET_1, SCISSOR_1, PRMODECONT, COLCLAMP, DTHE,
// TEST_1, each its value then its address.
void DefineDrawEnvironment(std::uint64_t* env, s16 psm, s16 w, s16 h, s16 ztest, s16 zpsm)
{
    env[1] = 0x4C;
    env[0] = ((std::uint64_t)(((w + 63) >> 6) & 0x3F) << 16) | ((std::uint64_t)(psm & 0xF) << 24);
    env[3] = 0x4E;
    env[2] = (std::uint64_t)(std::uint16_t)CountFrameBufferPages(psm, w, h) | ((std::uint64_t)(zpsm & 0xF) << 24) |
             (ztest == 0 ? (std::uint64_t)1 << 32 : 0);
    env[5] = 0x18;
    env[4] = (((std::uint64_t)2048 - (w >> 1)) << 4) | ((((std::uint64_t)2048 - (h >> 1)) << 4) << 32);
    env[7] = 0x40;
    env[6] = ((std::uint64_t)(w - 1) << 16) | ((std::uint64_t)(h - 1) << 48);
    env[9] = 0x1A;
    env[8] = 1;
    env[11] = 0x46;
    env[10] = 1;
    env[13] = 0x45;
    env[12] = (psm & 2) ? 1 : 0;
    env[15] = 0x47;
    env[14] = ztest != 0 ? (((std::uint64_t)(ztest & 3) << 17) | 0x10000) : 0x30000;
}

}  // namespace

void ResetGs(s16 mode, s16 x, s16 y, s16 flag)
{
    if (mode == 0 || mode == 5) {
        g_gsParams.interlace = x;
        g_gsParams.mode = y;
        g_gsParams.field = flag != 0;
    }
    if (mode == 0)
        WriteGsRegister(0x12001000, 0x200);
    else if (mode == 1)
        WriteGsRegister(0x12001000, 0x100);
}

// A display environment's words to the GS's privileged registers (PMODE, SMODE2, DISPFB2,
// DISPLAY2, BGCOLOR, the layout of the library's GS version), and to the GS model.
void WriteDisplayRegisters(void* self)
{
    const std::uint64_t* env = static_cast<const std::uint64_t*>(self);
    WriteGsRegister(0x12000000, env[0]);
    WriteGsRegister(0x12000020, env[1]);
    WriteGsRegister(0x12000090, env[2]);
    WriteGsRegister(0x120000A0, env[3]);
    WriteGsRegister(0x120000E0, env[4]);
    // in order with the drawing before it (the render thread, port/dmac.h)
    port::RenderCall([](std::uint64_t dispfb, std::uint64_t display) { port::gs::SetDisplay(dispfb, display); },
                     env[2], env[3]);
}

// A drawing environment's packet (its GIF tag and NLOOP quadwords) to the GS through the GIF
// channel.
s32 SendDrawEnvironment(void* self)
{
    std::uint64_t tag;
    std::memcpy(&tag, self, 8);
    std::uint32_t addr = port::GameAddr(self);
    if ((addr & 0x70000000u) == 0x70000000u)
        addr = (addr & 0x0FFFFFFFu) | 0x80000000u;
    port::SendGifNormal(addr, (std::uint32_t)(tag & 0x7FFF) + 1);
    return 0;
}

// The double buffer's two display environments, two drawing environments and their GIF tags
// (A+D, 8 registers; 14 with the clear, which the game does not ask for), and the second
// buffer's frame base: half the pages the two frames take.
void SetUpDoubleBuffer(void* self, s16 psm, s16 w, s16 h, s16 ztest, s16 zpsm, s16 flag)
{
    std::uint8_t* base = static_cast<std::uint8_t*>(self);
    auto at = [&](std::uint32_t off) { return reinterpret_cast<std::uint64_t*>(base + off); };
    DefineDisplayEnvironment(at(0x00), psm, w, h, 0, 0);
    DefineDisplayEnvironment(at(0x28), psm, w, h, 0, 0);
    DefineDrawEnvironment(at(0x60), psm, w, h, ztest, zpsm);
    DefineDrawEnvironment(at(0x150), psm, w, h, ztest, zpsm);
    for (std::uint32_t off : {0x50u, 0x140u}) {   // GIF tag: NLOOP 8 (14), EOP, PACKED, one register A+D
        at(off)[0] = (std::uint64_t)(flag ? 14 : 8) | (1ull << 15) | (1ull << 60);
        at(off)[1] = 0xE;
    }
    s32 pages = CountFrameBufferPages(psm, w, h);
    if ((g_gsParams.interlace == 1 && g_gsParams.field == 1) || g_gsParams.interlace == 0) {
        std::uint64_t half = (std::uint64_t)((s16)pages >> 1) & 0x1FF;
        at(0x38)[0] = (at(0x38)[0] & ~0x1FFull) | half;   // the second display shows the second frame
        at(0x60)[0] = (at(0x60)[0] & ~0x1FFull) | half;   // the first drawing environment draws into it
    }
}

// Shows one half of the double buffer and sends the other's drawing environment.
s32 SwapDoubleBuffer(void* self, s32 flag)
{
    std::uint8_t* base = static_cast<std::uint8_t*>(self);
    s32 idx = flag & 1;
    port::TimelineEvent("game swaps the buffers");
    WriteDisplayRegisters(base + idx * 0x28);
    s32 r = SendDrawEnvironment(base + (idx ? 0x140 : 0x50));
    port::RenderCall([](std::uint64_t, std::uint64_t) { port::gs::FrameShown(); }, 0, 0);
    return r;
}

s32 SwapGsInterruptMask(s32) { return 0; }

// A drawing environment's offset from its centre, half a line lower on the odd field.
void SetFieldDrawOffset(void* p, s16 a1, s16 a2, s16 a3)
{
    std::uint8_t* self = static_cast<std::uint8_t*>(p);
    std::uint64_t v;
    std::memcpy(&v, self + 0x30, 8);
    std::int64_t y = (std::int64_t)(((std::uint64_t)((std::int32_t)((v >> 48) & 0x7FF) + 1)) >> 1);
    std::int64_t x = (std::int64_t)(((std::uint64_t)((std::int32_t)((v >> 16) & 0x7FF) + 1)) >> 1);
    std::int64_t t = ((std::int64_t)a2 - y) << 4;
    std::uint64_t lo = (std::uint64_t)(((std::int64_t)a1 - x) << 4);
    std::uint64_t hi = (std::uint64_t)(a3 != 0 ? t + 8 : t) << 32;
    std::uint64_t out = lo | hi;
    std::memcpy(self + 0x20, &out, 8);
}

s32 GetGsInterruptMask(void) { return 0; }

// --- DMA: channels in game memory, transfers accepted

// The EE's ten DMA channels' register blocks (VIF0, VIF1, GIF, IPU from/to, SIF0-2, SPR from/to), at
// their hardware addresses (port/ptr32.h maps the register window to plain memory).
// (the game declares the result a pointer: the address is given as one, the word unchanged)
void* GetDmaChannel(u32 index)
{
    static const u32 kChannels[10] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                      0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(index < 10 ? kChannels[index] : 0));
}
s32 ResetDmaController(s32) { return 0; }
// As the library starts a source chain (TADR unless it holds 0xFFFFFFFF, QWC 0, CHCR's mode and STR),
// and the transfer then runs (port/dmac.h).
void StartDmaChain(void* ch, void* tadr)
{
    volatile u32* r = (volatile u32*)ch;
    std::uint32_t addr = port::GameAddr(tadr);
    if (addr - 0x70000000u < 0x4000u)   // the scratchpad, as a tag address names it
        addr = (addr & 0x3FF0u) | 0x80000000u;
    if (r[0x30 / 4] != 0xFFFFFFFFu)
        r[0x30 / 4] = addr;
    r[0x20 / 4] = 0;
    r[0] = (r[0] & ~0xCu) | 0x105u;
    port::RunDmaChain(port::GameAddr(ch), r[0x30 / 4]);
}
// As the library starts a destination chain (QWC 0, CHCR's mode, STR), and the transfer runs.
void StartDmaChainToMemory(void* ch)
{
    volatile u32* r = (volatile u32*)ch;
    r[0x20 / 4] = 0;
    r[0] = ((r[0] & ~0xCu) | 0x4u) & ~0x1u;
    r[0] |= 0x100u;
    port::RunFromSprChain(port::GameAddr(ch));
}
s32 WaitDmaChannel(void*, s32, s32) { return 0; }

}  // extern "C"
