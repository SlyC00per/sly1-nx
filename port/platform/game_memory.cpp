// SPDX-License-Identifier: AGPL-3.0-or-later
// The anchor of game addresses (include/port/ptr32.h): the program image's base on each
// platform, the scratchpad, and the slow paths of the conversions.
#include "port/ptr32.h"

#include <cstdio>
#include <cstdlib>

#if defined(__APPLE__)
#include <mach-o/ldsyms.h>
#elif defined(_WIN32)
#include <windows.h>
extern "C" IMAGE_DOS_HEADER __ImageBase;
#elif defined(__SWITCH__)
#include <switch.h>
#else
extern "C" char __executable_start[];
#endif

namespace port {

std::uintptr_t g_imageBase;

std::uintptr_t FindImageBase()
{
#if defined(__APPLE__)
    g_imageBase = reinterpret_cast<std::uintptr_t>(&_mh_execute_header);
#elif defined(_WIN32)
    g_imageBase = reinterpret_cast<std::uintptr_t>(&__ImageBase);
#elif defined(__SWITCH__)
    // libnx's linker script makes __start__ an absolute 0, which a position-independent program
    // does not relocate; the kernel says where the code segment holding this function begins,
    // and the image starts with its code.
    MemoryInfo info;
    u32 pageInfo;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, reinterpret_cast<u64>(&FindImageBase))))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_NotFound));
    g_imageBase = static_cast<std::uintptr_t>(info.addr);
#else
    g_imageBase = reinterpret_cast<std::uintptr_t>(__executable_start);
#endif
    return g_imageBase;
}

alignas(64) static unsigned char g_scratchpad[kScratchpadSize];

unsigned char* Scratchpad() { return g_scratchpad; }

void* NativeSlow(std::uint32_t addr)
{
    if (addr == 0)
        return nullptr;
    std::uint32_t seg = addr >> 28;
    std::uint32_t off = addr & 0x0FFFFFFFu;
    if ((seg == 0 || seg == 2 || seg == 3) && off != 0 && off < kGameMemorySize)
        return reinterpret_cast<void*>(ImageBase() + off);
    if (addr - kScratchpadAddr < kScratchpadSize)
        return g_scratchpad + (addr - kScratchpadAddr);
    return reinterpret_cast<void*>(kWordTag | addr);
}

std::uint32_t GameAddrSlow(const void* p)
{
    std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
    if (v == 0)
        return 0;
    if ((v & ~std::uintptr_t(0xFFFFFFFFu)) == kWordTag)
        return static_cast<std::uint32_t>(v);
    // A native pointer outside the image cannot be stored in a game structure: memory the game
    // reaches must come from its own arenas (docs/DESIGN.md, section 4).
    std::fprintf(stderr, "port: pointer %p is outside game memory (image base %p)\n", p,
                 reinterpret_cast<void*>(ImageBase()));
    std::abort();
}

}  // namespace port
