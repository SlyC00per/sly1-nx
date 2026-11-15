// SPDX-License-Identifier: AGPL-3.0-or-later
// The player's disc image: 2048-byte sectors read by number, as the game reads its data
// (docs/DISC.md in sly1-decomp: everything is found by sector), and the executable's bytes by
// their retail address, for the few the decompilation does not write in C (port/retail_data.h).
#ifndef PORT_ISO_H
#define PORT_ISO_H

#include <cstdint>
#include <string>

namespace port {

constexpr std::uint32_t kSectorSize = 2048;

// Opens the image; false (with a message) when it cannot be read or is not the NTSC-U disc.
bool OpenIso(const std::string& path);
bool IsoOpen();
// Reads count sectors from lsn; false past the image's end or on a read error.
bool ReadSectors(std::uint32_t lsn, std::uint32_t count, void* dst);
// Copies size bytes of the executable at retail address addr; false outside its loaded segments.
bool ReadExecutable(std::uint32_t addr, void* dst, std::uint32_t size);
// Copies the executable's loadable segments to their addresses in main memory (port_ram); false
// when there is no executable.
bool LoadExecutableSegments();

}  // namespace port

#endif
