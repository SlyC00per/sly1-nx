// SPDX-License-Identifier: AGPL-3.0-or-later
// The game's globals that its C declares but does not define (tools/gen_pointers.py turns each
// such declaration into a macro of the same name over RetailRef), and the memory they live in.
//
// RetailData(address) is the port's storage of a retail address:
//   0x27B300 to 0x2000000   the end of retail's image and its heap: one block (port_retail_ram)
//                           with every label at its retail offset; g_imageEnd (0x64C70F) and the
//                           heap's top (HeapEndAddr) fall where retail's do
//   0x211E10 to 0x217360    the VU microcode block: a placeholder, the port runs no microcode
//   inside a global the C   that global plus the offset (port_retail_data.cpp, written by
//   defines                 tools/gen.py from sly1-decomp's config/us/c_data.tsv)
//   anything else           an address used only as a number (the VU programs' addresses, the
//                           checksum ranges): the native pointer of that very game address, so
//                           that converting it back gives the number again
#ifndef PORT_RETAIL_DATA_H
#define PORT_RETAIL_DATA_H

#include <cstdint>

namespace port {

constexpr std::uint32_t kRetailRamBase = 0x0027B300;
constexpr std::uint32_t kRetailRamTop = 0x02000000;
constexpr std::uint32_t kVutextBase = 0x00211E10;
constexpr std::uint32_t kVutextSize = 0x5550;

void* RetailData(std::uint32_t addr);
template <class T> inline T& RetailRef(std::uint32_t addr) { return *reinterpret_cast<T*>(RetailData(addr)); }

// The heap's top as a game address: the end of port_retail_ram, where retail's 32 MB end is.
std::uint32_t HeapEndAddr();

}  // namespace port

#endif
