// SPDX-License-Identifier: AGPL-3.0-or-later
// VU1 (port/render/vu1.cpp): its registers, and an interpreter that runs the game's own VU1
// microcode (loaded by VIF1's MPG into micro memory, port/platform/dmac.cpp) when VIF1 starts a
// microprogram (MSCAL, MSCALF, MSCNT). Its memories are port::Vu1Memory(): micro memory (2,048
// instructions) and data memory (1,024 quadwords). What the microprograms send to the GS with
// XGKICK goes to the kick function the caller gives (GIF path 1).
#ifndef PORT_VU1_H
#define PORT_VU1_H

#include <cstdint>

namespace port {
namespace vu1 {

struct State {
    float vf[32][4];
    std::uint16_t vi[16];
    float acc[4];
    float q, p, i, r;
    std::uint32_t mac, status, clip;   // the flags as they stood when the last program ended
    std::uint32_t pc;                  // where MSCNT goes on (in instructions)
};

State& Vu1();

// The micro memory as 64-bit instructions (lower word, upper word), and the data memory.
std::uint32_t* Micro();
std::uint8_t* Data();

// VIF1's TOP and ITOP, which XTOP and XITOP read, and where XGKICK sends a packet (the quadword
// address of its first GIF tag in data memory).
struct Context {
    std::uint32_t top = 0, itop = 0;
    void (*kick)(std::uint32_t qaddr, void* user) = nullptr;
    void* user = nullptr;
};

// After VIF1's MPG wrote count instructions at addr: installs a translated chunk there when the
// upload is one (tools/vu1_translate.py), and drops any the upload overwrote.
void MicrocodeLoaded(std::uint32_t addr, std::uint32_t count);

// Runs the microprogram at instruction address pc until its end bit; gives the instructions run.
std::uint64_t Run(std::uint32_t pc, const Context& ctx);

// The instructions run in translated chunks so far.
std::uint64_t TranslatedInstructions();

}  // namespace vu1
}  // namespace port

#endif
