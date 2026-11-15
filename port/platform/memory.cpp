// SPDX-License-Identifier: AGPL-3.0-or-later
// The memory behind port/retail_data.h: retail's image end and heap, mirrored, and the lookup
// of a retail address.
#include "port/retail_data.h"

#include "port/ptr32.h"

namespace port {

// The labels of retail's .bss from 0x27B300 and the heap after them, to the PS2's 32 MB top, at
// their retail offsets. Zero, as retail's entry clears it (EnterProgram).
alignas(128) static unsigned char port_retail_ram[kRetailRamTop - kRetailRamBase];
alignas(16) static unsigned char port_vutext[kVutextSize];

// The labels inside globals the C defines (written by tools/gen.py).
struct RetailAlias {
    std::uint32_t addr;
    unsigned char* base;
    std::uint32_t offset;
};
extern const RetailAlias kRetailAliases[];
extern const int kRetailAliasCount;

// The labels whose bytes only the player's executable has (written by tools/gen.py).
struct RetailBytes {
    std::uint32_t addr;
    std::uint32_t offset;
    std::uint32_t size;
};
extern const RetailBytes kRetailBytes[];
extern const int kRetailBytesCount;
extern unsigned char kRetailBytesStorage[];

void* RetailData(std::uint32_t addr)
{
    if (addr >= kRetailRamBase && addr < kRetailRamTop)
        return port_retail_ram + (addr - kRetailRamBase);
    if (addr >= kVutextBase && addr < kVutextBase + kVutextSize)
        return port_vutext + (addr - kVutextBase);
    for (int i = 0; i < kRetailAliasCount; i++)
        if (kRetailAliases[i].addr == addr)
            return kRetailAliases[i].base + kRetailAliases[i].offset;
    for (int i = 0; i < kRetailBytesCount; i++)
        if (addr - kRetailBytes[i].addr < kRetailBytes[i].size)
            return kRetailBytesStorage + kRetailBytes[i].offset + (addr - kRetailBytes[i].addr);
    return Native(addr);
}

std::uint32_t HeapEndAddr() { return GameAddr(port_retail_ram + (kRetailRamTop - kRetailRamBase)); }

}  // namespace port
