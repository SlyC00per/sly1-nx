// SPDX-License-Identifier: AGPL-3.0-or-later
// The EE's DMA controller as the game drives it: a channel started in source-chain mode walks its
// tags at once, on the calling thread, feeding VIF0, VIF1 or the GIF; the devices behind them raise
// the interrupts the game waits on (port/platform/dmac.cpp). Written from the public descriptions
// of the PS2's DMA tags, VIFcodes and GIF tags.
#ifndef PORT_DMAC_H
#define PORT_DMAC_H

#include <cstdint>

namespace port {

// Runs a source chain on the channel whose register block is at regs (its game address, from
// GetDmaChannel), from tadr; the channel's CHCR says whether tags go to the VIF too (TTE). Returns
// when the chain has ended, with the channel stopped.
void RunDmaChain(std::uint32_t regs, std::uint32_t tadr);

// Runs a destination chain out of the scratchpad (channel 8), from its SADR.
void RunFromSprChain(std::uint32_t regs);

// A normal-mode transfer on the GIF channel: qwc quadwords from madr (bit 31: the scratchpad), as
// the graphics library's SendDrawEnvironment starts one.
void SendGifNormal(std::uint32_t madr, std::uint32_t qwc);

// The render thread (port/platform/dmac.cpp): its body, which a host thread on a stack in game
// memory runs (it may run the VIF's and the GS's interrupt handlers); fn(a, b) run in order with
// the transfers before it (the display's registers, the buffer swap); and a wait until the
// queue is empty. Without the thread (or with SLY1_RENDER_SYNC) everything runs at once.
void RenderThreadMain();
// The render thread's helper (the hashes of the model parts drawn again, a frame late): its body,
// started beside the GS thread. Without it every hash is made on the render thread.
void HashHelperMain();
// The counts of the model parts kept and drawn again so far, on one line (after each level of
// --verify-levels, to see a level's own by the difference).
void PartCountsLine(const char* what);
// The GS thread's body (the GS's packets and the display's calls, after the render thread's), on
// a stack in game memory too (it raises the GS's interrupt).
void GsThreadMain();
void RenderCall(void (*fn)(std::uint64_t, std::uint64_t), std::uint64_t a, std::uint64_t b);
void RenderFlush();
void TimelineEvent(const char* what);   // SLY1_TIMELINE (port/platform/dmac.cpp)

// What the transfers have done so far, for the run's report.
void DmacDump();

}  // namespace port

#endif
