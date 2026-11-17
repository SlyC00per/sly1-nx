// SPDX-License-Identifier: AGPL-3.0-or-later
// Native handlers for VU1's entry points (docs/RENDERER.md): each does what the game's microcode
// at one entry of one uploaded chunk does, from the same state to the same state (registers,
// flags, Q and P, data memory, the packets it kicks), without interpreting it. vu1::Run calls a
// handler where one is registered; SLY1_VU1_CHECK runs both and compares (port/render/vu1.cpp).
#ifndef PORT_RENDER_VU1_HANDLERS_H
#define PORT_RENDER_VU1_HANDLERS_H

#include <cstddef>
#include <cstdint>

#include "vu1_runner.h"

namespace port {
namespace vu1 {

// Runs the entry: leaves the state as the microprogram leaves it at its end (s.pc where MSCNT
// would go on, what later programs read), and kicks through r.ctx.kick as it would. Gives false,
// having changed nothing, for an input it leaves to the microcode.
typedef bool (*Handler)(core::Runner& r);

struct HandlerEntry {
    std::uint32_t chunkHash;   // the upload holding the entry (tools/vu1_translate.py's hash)
    std::uint32_t entry;       // the entry, in instructions
    Handler fn;
    const char* name;
    // What the entry leaves that a later program reads (SLY1_VU1_LIVE's report: the pieces it is
    // the last writer of when an entry reads them), in port/render/vu1_live.h's names. The handler
    // leaves these as the microcode does; the other registers it writes are scratch.
    const char* leaves;
};

// Before a program runs (fn its handler, or null) and when a handler left its entry to the
// microcode: a batch's transform noted for the GPU renderer is done now if what follows is not
// one of the batch's kicks (port/render/vu1_handlers.cpp, "left to the GPU").
void BeforeProgram(Handler fn, core::Runner& r);
void LeftToMicrocode(core::Runner& r);

// The batches' kicks the GPU renderer took, and those it did not (done by the program after all).
void BatchCounts(std::uint64_t* taken, std::uint64_t* notTaken);

extern const HandlerEntry kHandlers[];
extern const std::size_t kHandlerCount;

}  // namespace vu1
}  // namespace port

#endif
