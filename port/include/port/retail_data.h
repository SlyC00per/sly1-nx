// SPDX-License-Identifier: AGPL-3.0-or-later
// The game's globals, at their retail addresses in the mirrored main memory (port/ptr32.h).
//
// tools/gen.py turns every declaration of a game global (D_XXXXXXXX, whatever readable name it is
// spelled with) into a macro of the same name over RetailRef, with that declaration's own type,
// and every definition into its initial value (port_init_D_XXXXXXXX), which start-up copies to
// the global's place. Start-up first fills main memory from the player's executable
// (LoadRetailImage), so every byte the decompilation does not write in C has its retail value.
// An address used only as a number (a VU program's, a checksum range's end) is simply that
// address.
#ifndef PORT_RETAIL_DATA_H
#define PORT_RETAIL_DATA_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "port/ptr32.h"

namespace port {

template <class T> inline T& RetailRef(std::uint32_t addr) { return *reinterpret_cast<T*>(Native(addr)); }

// Copies a global's initial value to its retail address (written by tools/gen.py, for each
// definition).
inline void RetailInit(std::uint32_t addr, const void* value, std::size_t size)
{
    std::memcpy(Native(addr), value, size);
}

// Fills main memory with the executable's loadable segments (port/iso.h); false without one.
bool LoadRetailImage();

// Preemption points (port/platform/kernel.cpp). tools/gen.py puts Preempt() in every loop test of
// the game's code: when an interrupt has readied a thread of higher priority, the running thread
// gives way there, as the PS2 preempts it. Spin() is an empty wait loop's body: the same, and a
// moment for the host's other threads.
extern std::atomic<bool> g_preemptPending;
void PreemptSlow();
inline void Preempt()
{
    if (g_preemptPending.load(std::memory_order_relaxed))
        PreemptSlow();
}
void Spin();

// A local the game views through a larger type (its address cast to a Mat3* and written whole):
// the local with N bytes of room, its own type and fields as they were (tools/gen_pointers.py,
// roomy_cast). Retail's frame gave it that room in its neighbours and padding.
template <class T, std::size_t N> struct alignas(16) Room : T {
    char port_room[N - sizeof(T)];
    // Initialised as the local was: from a value of its type, from a braced list of its fields,
    // or not at all (zeroed here).
    template <class... A> Room(A... a) : T{a...}, port_room{} {}
    Room& operator=(const T& v)
    {
        static_cast<T&>(*this) = v;
        return *this;
    }
};

}  // namespace port

#endif
