// SPDX-License-Identifier: AGPL-3.0-or-later
// The platform layer: the disc, pad, GS and DMA libraries the game calls (sly1-decomp src/lib's
// cdvd.c, pad.c, graph.c and dma.c; their roles in config/names/lib).
//
// Disc: a read is done when it is started, from the player's image (port/iso.h); every status
//   call reports a ready drive with no error.
// Pad: no controller yet (milestone 2 maps the Switch's): every slot reports closed.
// GS and DMA: the port's renderer takes the game's packets at the DMA calls (milestone 4); until
//   then they are accepted and dropped. A DMA channel the game asks for is a block of memory of
//   the channel's register layout inside game memory, so the game's writes to it land somewhere
//   harmless and its reads find a channel that is never busy.
#include <cstdio>
#include <cstring>

#include "common.h"
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

// --- the pad: no controller yet

s32 InitPadLibrary(s32) { return 1; }
s32 OpenPadPort(s32, s32, u8*) { return 1; }
s32 ReadPadFrame(s32, s32, u8*) { return 0; }
s32 GetPadSlotState(s32, s32) { return 0x63; }       // closed
s32 GetPadRequestMode(s32, s32) { return 0; }
s32 GetPadReplyTableByte(s32, s32, s32, s32) { return 0; }
s32 GetPadReplyListValue(s32, s32, s32, s32) { return 0; }
s32 SendPadTwoWordRequest(s32, s32, s32, s32) { return 0; }
s32 SetPadActuatorBytes(s32, s32, u8*) { return 0; }
s32 SendPadSixByteRequest(s32, s32, u8*) { return 0; }
s32 HasFullPadMask(s32, s32) { return 0; }
s32 SendFullPadMask(s32, s32) { return 0; }

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
void StartDmaChain(void*, void*) {}
void StartDmaChainToMemory(void*) {}
s32 WaitDmaChannel(void*, s32, s32) { return 0; }

}  // extern "C"
