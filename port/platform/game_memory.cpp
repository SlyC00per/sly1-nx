// SPDX-License-Identifier: AGPL-3.0-or-later
// The anchor of game addresses (include/port/ptr32.h): the program image's base on each
// platform, the scratchpad, and the slow paths of the conversions.
#include "port/ptr32.h"
#include "port/vu0_micro.h"

#include <set>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <mutex>
#include <vector>
#include <cstdlib>
#include <cstring>

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
    if (size == 0 || size >= kGameMemorySize) {
        std::fprintf(stderr, "port: the program image spans 0x%llX bytes; game addresses reach 0x%X\n",
                     (unsigned long long)size, (unsigned)kGameMemorySize);
        std::_Exit(70);
    }
    g_imageSize = size;
    return g_imageBase;
}

alignas(64) static unsigned char g_scratchpad[kScratchpadSize];

}  // namespace port

// The PS2's main memory, mirrored: the game's globals at their retail addresses, filled from the
// player's executable and the decompilation's initialisers (port/retail_data.h), and its heap.
// With a guard after it: main memory's end address is a game address too (the heap's top), so
// GameAddr takes a pointer one past port_ram as 0x02000000; no other object may start there (the
// Switch's linker put the scratchpad there, which then came back as 0x02000000).
alignas(4096) unsigned char port_ram[port::kRamSize + 4096];

namespace port {

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

// The vector units' memories as the EE sees them, 0x11000000 to 0x11010000: VU0's micro and data
// memory are the interpreter's (port/vu0_micro.h); VU1's (micro from 0x11008000, data from
// 0x1100C000, 16 KiB each) are plain memory until the renderer takes them.
constexpr std::uint32_t kVuMemory = 0x11000000, kVuMemorySize = 0x10000;
static unsigned char* g_vu1Memory;   // 0x11008000 to 0x11010000

static void* VuWindow(std::uint32_t off)
{
    vu0::State& s = vu0::Vu0();
    if (off < 0x1000)
        return reinterpret_cast<unsigned char*>(s.micro) + off;
    if (off - 0x4000 < 0x1000)
        return s.data + (off - 0x4000);
    if (off >= 0x8000)
        return Registers(g_vu1Memory, 0x8000) + (off - 0x8000);
    return nullptr;
}

// VU1's micro memory (16 KiB) and data memory (16 KiB, from +0x4000), for the renderer's VIF1 and
// VU1 (port/vu1.h).
unsigned char* Vu1Memory() { return Registers(g_vu1Memory, 0x8000); }

static bool VuAddr(std::uintptr_t v, std::uint32_t* addr)
{
    vu0::State& s = vu0::Vu0();
    std::uintptr_t micro = reinterpret_cast<std::uintptr_t>(s.micro), data = reinterpret_cast<std::uintptr_t>(s.data);
    if (v - micro < 0x1000) { *addr = kVuMemory + (std::uint32_t)(v - micro); return true; }
    if (v - data < 0x1000) { *addr = kVuMemory + 0x4000 + (std::uint32_t)(v - data); return true; }
    std::uintptr_t vu1 = reinterpret_cast<std::uintptr_t>(g_vu1Memory);
    if (g_vu1Memory && v - vu1 < 0x8000) { *addr = kVuMemory + 0x8000 + (std::uint32_t)(v - vu1); return true; }
    return false;
}


void* NativeSlow(std::uint32_t addr)
{
    if (addr == 0)
        return nullptr;
    if (addr - kEeRegisters < kEeRegistersSize)
        return Registers(g_eeRegisters, kEeRegistersSize) + (addr - kEeRegisters);
    if (addr - kGsRegisters < kGsRegistersSize)
        return Registers(g_gsRegisters, kGsRegistersSize) + (addr - kGsRegisters);
    if ((addr >> 28) == 2 && (addr & 0x0FFFFFFFu) < kRamSize)
        return port_ram + (addr & 0x0FFFFFFFu);   // the uncached alias of main memory
    if (addr - kScratchpadAddr < kScratchpadSize)
        return g_scratchpad + (addr - kScratchpadAddr);
    if (addr - kVuMemory < kVuMemorySize)
        if (void* p = VuWindow(addr - kVuMemory))
            return p;
    // An address of nothing (a plain word the game keeps where a declaration says pointer comes
    // here too, and is never read): the first above main memory's end is reported with its caller,
    // which a read would fault in.
    // SLY1_ADDR_TRACE=1: each place that asks for an address of nothing, once (a pointer cut to 32
    // bits and handed on as a game address is found this way before it is read).
    static const bool trace = std::getenv("SLY1_ADDR_TRACE") != nullptr;
    if (trace) {
        static std::mutex lock;
        static std::set<std::uintptr_t> seen;
        const std::uintptr_t from = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) - ImageBase();
        std::lock_guard<std::mutex> lk(lock);
        if (seen.size() < 200 && seen.insert(from).second)
            std::fprintf(stderr, "addrtrace: game address 0x%08X maps to nothing; from image +0x%llX\n", addr, (unsigned long long)from);
    }
    static bool reported;
    if (!reported && addr - kRamSize < 0x00100000u) {
        reported = true;
        std::fprintf(stderr, "port: game address 0x%08X maps to nothing; from image +0x%llX\n", addr,
                     (unsigned long long)(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) - ImageBase()));
#if defined(__SWITCH__)
        // the callers, by the frame records (the Switch build keeps frame pointers)
        auto* fp = static_cast<std::uintptr_t*>(__builtin_frame_address(0));
        for (int i = 0; i < 12 && fp && reinterpret_cast<std::uintptr_t>(fp) % 8 == 0; i++) {
            std::uintptr_t lr = fp[1];
            if (lr - ImageBase() >= ImageSize())
                break;
            std::fprintf(stderr, "port:   called from image +0x%llX\n", (unsigned long long)(lr - ImageBase()));
            fp = reinterpret_cast<std::uintptr_t*>(fp[0]);
        }
        // Reading it would fault, and a fault in the emulator leaves Ryujinx unable to exit: the run
        // ends here instead, its report made.
        std::fflush(stderr);
        static const char line[] = "sly1: done, status 4";
        svcOutputDebugString(line, sizeof line - 1);
        std::_Exit(4);
#endif
    }
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
    // (the same for the PS2's own register blocks, the EE's and VU memory at 0x10000000 to 0x1100FFFF and
    // the GS's at 0x12000000: a DMA channel's address held as a pointer. Ryujinx loads the program
    // below them, at 0x8500000, where the Switch loads it above: the test above let them by there only)
    if (v <= 0xFFFFFFFFu && ((v >= 0x10000000u && v < 0x11010000u) || (v >= 0x12000000u && v < 0x12002000u)))
        return static_cast<std::uint32_t>(v);
    if (g_eeRegisters && v - reinterpret_cast<std::uintptr_t>(g_eeRegisters) < kEeRegistersSize)
        return kEeRegisters + static_cast<std::uint32_t>(v - reinterpret_cast<std::uintptr_t>(g_eeRegisters));
    if (g_gsRegisters && v - reinterpret_cast<std::uintptr_t>(g_gsRegisters) < kGsRegistersSize)
        return kGsRegisters + static_cast<std::uint32_t>(v - reinterpret_cast<std::uintptr_t>(g_gsRegisters));
    std::uint32_t vu;
    if (VuAddr(v, &vu))
        return vu;
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

// --- the game's functions by retail address (port/ptr32.h)

namespace {

// Built on first use, which may be during static initialisation (a stored function pointer's
// initial value): a function-local static is constructed then, where a global might be constructed
// again afterwards.
struct FunctionIndex {
    std::vector<FunctionEntry> byAddr, byFn;

    FunctionIndex()
    {
        for (std::size_t i = 0; i < port_function_table_count; i++) {
            const FunctionTable& table = port_function_tables[i];
            if (table.entries)
                byAddr.insert(byAddr.end(), table.entries, table.entries + table.count);
            for (const NamedFunction* n = table.named; n && n->name; n++) {
                unsigned addr;
                if (std::sscanf(n->name, "func_%8X", &addr) != 1) {
                    std::fprintf(stderr, "port: %s is not a game function's name\n", n->name);
                    std::abort();
                }
                byAddr.push_back({addr, n->fn});
            }
        }
        byFn = byAddr;
        std::sort(byAddr.begin(), byAddr.end(),
                  [](const FunctionEntry& a, const FunctionEntry& b) { return a.addr < b.addr; });
        std::sort(byFn.begin(), byFn.end(), [](const FunctionEntry& a, const FunctionEntry& b) {
            return std::less<const void*>()(a.fn, b.fn);
        });
    }
};

const FunctionIndex& Functions()
{
    static const FunctionIndex index;
    return index;
}

}  // namespace

void* FindFunction(std::uint32_t addr)
{
    const FunctionIndex& index = Functions();
    auto it = std::lower_bound(index.byAddr.begin(), index.byAddr.end(), addr,
                               [](const FunctionEntry& e, std::uint32_t a) { return e.addr < a; });
    return it != index.byAddr.end() && it->addr == addr ? it->fn : nullptr;
}

void* NativeFunction(std::uint32_t addr)
{
    // Retail's code lies from 0x00100000 for 0x00150000 bytes, a function at every fourth byte at
    // most: one table by address, looked at once. (The search of the sorted list, at every call
    // through a pointer, was 4% of the game's main thread on the Switch.)
    static const std::vector<void*>& direct = *[] {
        auto* table = new std::vector<void*>(0x00150000u / 4, nullptr);
        for (const FunctionEntry& e : Functions().byAddr)
            if (e.addr - 0x00100000u < 0x00150000u && (e.addr & 3) == 0)
                (*table)[(e.addr - 0x00100000u) / 4] = e.fn;
        return table;
    }();
    if (addr - 0x00100000u < 0x00150000u && (addr & 3) == 0)
        if (void* fn = direct[(addr - 0x00100000u) / 4])
            return fn;
    const FunctionIndex& index = Functions();
    auto it = std::lower_bound(index.byAddr.begin(), index.byAddr.end(), addr,
                               [](const FunctionEntry& e, std::uint32_t a) { return e.addr < a; });
    if (it != index.byAddr.end() && it->addr == addr)
        return it->fn;
    // An address in retail's code with no function: the call would run data. Said once per address.
    void* native = Native(addr);
    std::uintptr_t off = (std::uintptr_t)native - (std::uintptr_t)port_ram;
    if (off - 0x00100000u < 0x00150000u) {
        static std::mutex lock;
        static std::vector<std::uint32_t> said;
        std::lock_guard<std::mutex> lk(lock);
        if (std::find(said.begin(), said.end(), addr) == said.end()) {
            said.push_back(addr);
            std::fprintf(stderr, "port: no function at retail address 0x%08X (%zu in the table)\n", addr,
                         index.byAddr.size());
        }
    }
    return native;
}

std::uint32_t FunctionAddr(const void* fn)
{
    const FunctionIndex& index = Functions();
    auto it = std::lower_bound(index.byFn.begin(), index.byFn.end(), fn, [](const FunctionEntry& e, const void* f) {
        return std::less<const void*>()(e.fn, f);
    });
    if (it != index.byFn.end() && it->fn == fn)
        return it->addr;
    return GameAddr(fn);
}

// --- writable copies of the game's stored string literals (port/ptr32.h)

char* WritableString(const char* literal)
{
    // Built during static initialisation, before the game's threads; function-local, so that it
    // exists whenever the first initialiser asks.
    static std::mutex lock;
    static std::vector<std::pair<const char*, char*>> copies;
    static char pool[256 * 1024];
    static std::size_t used;
    std::lock_guard<std::mutex> lk(lock);
    for (auto& c : copies)
        if (c.first == literal)
            return c.second;
    std::size_t n = std::strlen(literal) + 1;
    if (used + n > sizeof pool) {
        std::fprintf(stderr, "port: the pool of the game's strings is full\n");
        std::abort();
    }
    char* copy = pool + used;
    std::memcpy(copy, literal, n);
    used += (n + 3) & ~std::size_t(3);
    copies.emplace_back(literal, copy);
    return copy;
}

}  // namespace port
