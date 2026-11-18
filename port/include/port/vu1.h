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

// VIF1 unpacked to qaddr of the data memory (port/platform/dmac.cpp): a batch whose
// transform was left to the GPU renderer is no longer the one in memory when they are its own
// (port/render/vu1_handlers.cpp).
// count quadwords written in cycles: write of every cycle quadwords (all of them when write is 0 or
// not below cycle), VIF1's CYCLE.
void DataWritten(std::uint32_t qaddr, std::uint32_t count, std::uint32_t cycle, std::uint32_t write);

// For a model part drawn again from what was kept of its packet (port/platform/dmac.cpp; the
// functions are in port/render/vu1_handlers.cpp): the matrix its batches are kicked with, from
// the header at top and the matrix at 4-7 of data memory; a count that grows when a batch was
// not handed over as it was unpacked; the packet not run; ids for count batches.
void PartMatrix(std::uint32_t top, float out[16]);
// The same from a header's first four quadwords held elsewhere (a part whose header is unpacked
// inside its own packet).
void PartMatrixFrom(const float* header, float out[16]);
std::uint64_t PartTrouble();
void PartSkipped();
std::uint32_t TakeBatchIds(std::uint32_t count);

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

// The handler calls checked against the interpreter (SLY1_VU1_CHECK), and how many differed.
std::uint64_t HandlerChecks(std::uint64_t* failures);

// SLY1_VU1_LIVE's findings (port/render/vu1_live.h), written to stderr when it is on.
void ReportLive();

// The instructions run in translated chunks so far.
std::uint64_t TranslatedInstructions();

// The program running now (or last): its upload's hash and its entry, for diagnosis.
void CurrentProgram(std::uint32_t* hash, std::uint32_t* entry);

}  // namespace vu1
}  // namespace port

#endif
