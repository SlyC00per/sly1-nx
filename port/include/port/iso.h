// SPDX-License-Identifier: AGPL-3.0-or-later
// The player's disc image: 2048-byte sectors read by number, as the game reads its data
// (docs/DISC.md in sly1-decomp: everything is found by sector), and the executable's bytes by
// their retail address, for the few the decompilation does not write in C (port/retail_data.h).
#ifndef PORT_ISO_H
#define PORT_ISO_H

#include <cstdint>
#include <string>
#include <vector>

namespace port {

constexpr std::uint32_t kSectorSize = 2048;

// Opens the image; false (with a message) when it cannot be read or is not the NTSC-U disc.
bool OpenIso(const std::string& path);
// What a file is, by what is in it and not by its name: the disc the port is made for is the one
// whose root directory has SCUS_971.98 with the SHA-1 57dc305db57932ad3f1122966cdb695d2e62a47a
// (sly1-decomp config/versions.json). Hashing the executable takes a fraction of a second; the
// whole image (2.8 GB) is not hashed.
enum class DiscKind {
    Good,           // the NTSC-U disc
    Unreadable,     // cannot be opened or read
    NotADisc,       // no ISO 9660 volume in it
    OtherGame,      // a disc with no Sly Cooper executable in its root directory
    OtherVersion,   // Sly Cooper of another region or revision (its executable's name in what)
    Altered,        // SCUS_971.98 with other contents: a patched or damaged image
};
DiscKind IdentifyDisc(const std::string& path, std::string* what = nullptr);
// A line for the player saying why a file of that kind is not used.
const char* DiscKindReason(DiscKind kind);
// The files of a folder (not its folders), each with its kind; found is the first good one.
struct DiscCandidate {
    std::string name;
    DiscKind kind;
    std::string what;
};
std::string FindDisc(const std::string& folder, std::vector<DiscCandidate>* seen);
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
