// SPDX-License-Identifier: AGPL-3.0-or-later
// The anchor of game addresses (include/port/ptr32.h): the program image's base on each
// platform, the scratchpad, and the slow paths of the conversions.
#include "port/ptr32.h"

#include <cstdio>
#include <cstdlib>

#if defined(__APPLE__) || defined(__linux__)
#include <execinfo.h>
#define PORT_HAVE_BACKTRACE 1
#endif

#if defined(__APPLE__)
#include <mach-o/ldsyms.h>
#include <mach-o/loader.h>
#include <cstring>
#elif defined(_WIN32)
#include <windows.h>
extern "C" IMAGE_DOS_HEADER __ImageBase;
#elif defined(__SWITCH__)
#include <switch.h>
#else
extern "C" char __executable_start[];
extern "C" char _end[];
#endif

namespace port {

std::uintptr_t g_imageBase;
std::uintptr_t g_imageSize;

std::uintptr_t FindImageBase()
{
    std::uintptr_t size = 0;
#if defined(__APPLE__)
    // The extent of the executable's segments from __TEXT (where the header is), __LINKEDIT and
    // __PAGEZERO aside.
    const mach_header_64* h = &_mh_execute_header;
    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(h), textVm = 0, end = 0;
    const load_command* lc = reinterpret_cast<const load_command*>(h + 1);
    for (std::uint32_t i = 0; i < h->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const segment_command_64* s = reinterpret_cast<const segment_command_64*>(lc);
            if (!std::strcmp(s->segname, "__TEXT"))
                textVm = s->vmaddr;
            else if (std::strcmp(s->segname, "__PAGEZERO") && std::strcmp(s->segname, "__LINKEDIT") &&
                     s->vmaddr + s->vmsize > end)
                end = s->vmaddr + s->vmsize;
        }
        lc = reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(lc) + lc->cmdsize);
    }
    g_imageBase = base;
    size = end - textVm;
#elif defined(_WIN32)
    g_imageBase = reinterpret_cast<std::uintptr_t>(&__ImageBase);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(&__ImageBase) + __ImageBase.e_lfanew);
    size = nt->OptionalHeader.SizeOfImage;
#elif defined(__SWITCH__)
    // libnx's linker script makes __start__ an absolute 0, which a position-independent program
    // does not relocate; the kernel says where the code segment holding this function begins,
    // and the image starts with its code.
    MemoryInfo info;
    u32 pageInfo;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, reinterpret_cast<u64>(&FindImageBase))))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_NotFound));
    g_imageBase = static_cast<std::uintptr_t>(info.addr);
    // The module's regions follow each other from its code: code, read-only data, data and .bss.
    std::uintptr_t at = g_imageBase;
    for (;;) {
        if (R_FAILED(svcQueryMemory(&info, &pageInfo, at)) || info.type == MemType_Unmapped ||
            info.addr + info.size <= at)
            break;
        at = info.addr + info.size;
    }
    size = at - g_imageBase;
#else
    g_imageBase = reinterpret_cast<std::uintptr_t>(__executable_start);
    size = reinterpret_cast<std::uintptr_t>(_end) - g_imageBase;
#endif
    if (size == 0 || size > kGameMemorySize) {
        std::fprintf(stderr, "port: the program image spans 0x%llX bytes; game addresses reach 0x%X\n",
                     (unsigned long long)size, (unsigned)kGameMemorySize);
        std::_Exit(70);
    }
    g_imageSize = size;
    return g_imageBase;
}

alignas(64) static unsigned char g_scratchpad[kScratchpadSize];

unsigned char* Scratchpad() { return g_scratchpad; }

// The hardware registers the game reads and writes (the EE's from 0x10000000: timers, DMA
// channels, VIF, IPU; the GS's privileged ones from 0x12000000): plain memory, where a write
// lands and a read finds the last value written. Outside the image on purpose, so that a pointer
// into it converts back to its hardware address on the slow path, which the fast path never
// pays for. A register that needs behaviour of its own gets it in the platform layer.
constexpr std::uint32_t kEeRegisters = 0x10000000, kEeRegistersSize = 0x10000;
constexpr std::uint32_t kGsRegisters = 0x12000000, kGsRegistersSize = 0x2000;
static unsigned char* g_eeRegisters;
static unsigned char* g_gsRegisters;

static unsigned char* Registers(unsigned char*& block, std::uint32_t size)
{
    if (!block)
        block = static_cast<unsigned char*>(std::calloc(1, size));
    return block;
}

void* NativeSlow(std::uint32_t addr)
{
    if (addr == 0)
        return nullptr;
    if (addr - kEeRegisters < kEeRegistersSize)
        return Registers(g_eeRegisters, kEeRegistersSize) + (addr - kEeRegisters);
    if (addr - kGsRegisters < kGsRegistersSize)
        return Registers(g_gsRegisters, kGsRegistersSize) + (addr - kGsRegisters);
    std::uint32_t seg = addr >> 28;
    std::uint32_t off = addr & 0x0FFFFFFFu;
    if ((seg == 0 || seg == 2) && off != 0 && off < kGameMemorySize)
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
    // A plain word that arrived as a pointer: one file's declaration says pointer where the
    // function returns or takes an integer (a semaphore's id), harmless on the PS2, where a pointer
    // is a 32-bit word. No host pointer lies below the image, so this hides no real error.
    if (v < ImageBase() && v <= 0xFFFFFFFFu)
        return static_cast<std::uint32_t>(v);
    if (g_eeRegisters && v - reinterpret_cast<std::uintptr_t>(g_eeRegisters) < kEeRegistersSize)
        return kEeRegisters + static_cast<std::uint32_t>(v - reinterpret_cast<std::uintptr_t>(g_eeRegisters));
    if (g_gsRegisters && v - reinterpret_cast<std::uintptr_t>(g_gsRegisters) < kGsRegistersSize)
        return kGsRegisters + static_cast<std::uint32_t>(v - reinterpret_cast<std::uintptr_t>(g_gsRegisters));
    // A native pointer outside the image cannot be stored in a game structure: memory the game
    // reaches must come from its own arenas (docs/DESIGN.md, section 4).
    std::fprintf(stderr, "port: pointer %p is outside game memory (image base %p, offset 0x%llX)\n", p,
                 reinterpret_cast<void*>(ImageBase()), (unsigned long long)(v - ImageBase()));
#ifdef PORT_HAVE_BACKTRACE
    void* frames[32];
    int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, 2);
#endif
    std::fflush(stderr);
    std::_Exit(70);
}

}  // namespace port
