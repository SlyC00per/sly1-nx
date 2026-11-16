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

// --- the GS: display set-up, accepted

void ResetGs(s16, s16, s16, s16) {}
void WriteDisplayRegisters(void*) {}
s32 SendDrawEnvironment(void*) { return 0; }
void SetUpDoubleBuffer(void*, s16, s16, s16, s16, s16, s16) {}
s32 SwapDoubleBuffer(void*, s32) { return 0; }
s32 SwapGsInterruptMask(s32) { return 0; }
void SetFieldDrawOffset(void*, s16, s16, s16) {}
s32 GetGsInterruptMask(void) { return 0; }

// --- DMA: channels in game memory, transfers accepted

// The EE's ten DMA channels' register blocks (VIF0, VIF1, GIF, IPU from/to, SIF0-2, SPR from/to), at
// their hardware addresses (port/ptr32.h maps the register window to plain memory).
u32 GetDmaChannel(u32 index)
{
    static const u32 kChannels[10] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                      0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};
    return index < 10 ? kChannels[index] : 0;
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
