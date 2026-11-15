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

// What the transfers have done so far, for the run's report.
void DmacDump();

}  // namespace port

#endif
