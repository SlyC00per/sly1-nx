// SPDX-License-Identifier: AGPL-3.0-or-later
// What VU1's programs carry from one to the next (SLY1_VU1_LIVE): while the interpreter runs, each
// instruction's reads and writes are decoded and every piece of state an entry reads before it
// writes it (a field of a VF register, a VI register, ACC, Q, P, I, the flags) is recorded as
// that entry's input. The union over all entries is the state a native handler must leave as the
// microcode leaves it (port/render/vu1_handlers.h); the rest is the programs' scratch.
#ifndef PORT_RENDER_VU1_LIVE_H
#define PORT_RENDER_VU1_LIVE_H

#include <bitset>
#include <cstdint>

namespace port {
namespace vu1 {
namespace live {

// The pieces of state, as bit numbers.
enum : int {
    kVf = 0,          // vf r, field k (x 0 .. w 3): kVf + r * 4 + k
    kVi = 128,        // vi r: kVi + r
    kAcc = 144,       // ACC field k
    kQ = 148, kP, kI, kMac, kStatus, kSticky, kClip,
    kCount
};
using Set = std::bitset<kCount>;

bool Enabled();

// A program starts at entry (of the chunk with hash chunk, 0 when none is installed).
void Begin(std::uint32_t entry, std::uint32_t chunk);

// Before the instruction at pc (upper word hi, lower word lo) runs at cycle.
void Instruction(std::uint32_t pc, std::uint32_t hi, std::uint32_t lo, std::uint64_t cycle);

// The union of what any entry read before writing it so far.
const Set& Inputs();

// Each entry's inputs, and the union, written to stderr.
void Report();

// The pieces a list names, as the report writes them ("vf09.zw vf19.z Q clip"); aborts on a
// name it does not know.
Set Parse(const char* names);

// A piece's name (vf12.x, vi04, ACC.y, Q ...).
const char* Name(int bit, char* buf, int size);

}  // namespace live
}  // namespace vu1
}  // namespace port

#endif
