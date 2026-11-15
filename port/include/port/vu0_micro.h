// SPDX-License-Identifier: AGPL-3.0-or-later
// VU0 in micro mode (port/platform/vu0_micro.cpp): its registers and memories, and an interpreter
// that runs the game's own microcode (loaded by VIF0's MPG, port/platform/dmac.cpp) when the game
// starts a microprogram (vcallms, VIF0's MSCAL). The game's VU0 microcode is three small programs:
// the particle filter (0x010) and the two light tests (0x060, 0x168).
#ifndef PORT_VU0_MICRO_H
#define PORT_VU0_MICRO_H

#include <cstdint>

namespace port {
namespace vu0 {

struct State {
    float vf[32][4];
    std::uint16_t vi[16];
    float acc[4];
    float q, i;
    std::uint32_t mac;    // the MAC flags the last arithmetic left
    std::uint32_t pc;     // where MSCNT goes on (in 64-bit instructions)
    alignas(16) std::uint8_t data[4096];
    std::uint32_t micro[1024];   // 512 instructions: lower word, upper word
};

State& Vu0();

// Runs the microprogram at byte address addr until its end bit.
void Run(std::uint32_t addr);

}  // namespace vu0
}  // namespace port

#endif
