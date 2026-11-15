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

}  // namespace port

#endif
