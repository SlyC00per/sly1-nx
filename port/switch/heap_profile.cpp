// SPDX-License-Identifier: AGPL-3.0-or-later
// Diagnosis only (cmake -DSLY1_HEAP_PROFILE=ON): the global operator new and delete replaced, so that
// the bytes each place in the program has asked for and not yet given back are counted, by the
// address that called new. port::HeapProfileReport prints the places holding the most (their
// addresses from the image's start, for addr2line against the ELF). Aligned news are not counted.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "port/ptr32.h"

namespace {
constexpr std::size_t kSites = 1 << 14;
struct Site {
    std::atomic<std::uintptr_t> pc{0};   // (a hash of the four return addresses)
    std::uintptr_t chain[4] = {};
    std::atomic<std::int64_t> bytes{0}, blocks{0};
};
Site g_sites[kSites];
std::atomic<std::int64_t> g_untracked{0};

struct alignas(16) Header {
    std::uint64_t size;
    std::uint32_t site;
    std::uint32_t magic;
};
constexpr std::uint32_t kMagic = 0x5EA70C11u;

std::uint32_t SiteFor(std::uintptr_t pc, const std::uintptr_t* chain)
{
    std::size_t i = (pc * 0x9E3779B97F4A7C15ull) >> 50;
    for (std::size_t n = 0; n < kSites; n++, i = (i + 1) & (kSites - 1)) {
        std::uintptr_t was = g_sites[i].pc.load(std::memory_order_relaxed);
        if (was == pc)
            return (std::uint32_t)i;
        if (was == 0) {
            std::uintptr_t zero = 0;
            if (g_sites[i].pc.compare_exchange_strong(zero, pc)) {
                for (int k = 0; k < 4; k++)
                    g_sites[i].chain[k] = chain[k];
                return (std::uint32_t)i;
            }
            if (zero == pc)
                return (std::uint32_t)i;
        }
    }
    return kSites;   // (full)
}

// The return addresses of the four frames above new, by the frame records (the build with
// -fno-omit-frame-pointer); their hash names the place.
__attribute__((noinline)) void Chain(std::uintptr_t* out)
{
    const std::uintptr_t* fp = static_cast<const std::uintptr_t*>(__builtin_frame_address(0));
    for (int k = 0; k < 6; k++) {
        const std::uintptr_t lr = fp ? fp[1] : 0;
        if (k >= 2)
            out[k - 2] = lr;
        if (!fp || (fp[0] & 7) != 0 || fp[0] <= (std::uintptr_t)fp)
            fp = nullptr;
        else
            fp = reinterpret_cast<const std::uintptr_t*>(fp[0]);
    }
}

void* Take(std::size_t size, std::uintptr_t)
{
    std::uintptr_t chain[4] = {};
    Chain(chain);
    std::uintptr_t pc = 0;
    for (int k = 0; k < 4; k++)
        pc = (pc ^ chain[k]) * 0x100000001B3ull;
    pc |= 1;
    Header* h = static_cast<Header*>(std::malloc(size + sizeof(Header)));
    if (!h)
        return nullptr;
    h->size = size;
    h->magic = kMagic;
    h->site = SiteFor(pc, chain);
    if (h->site < kSites) {
        g_sites[h->site].bytes.fetch_add((std::int64_t)size, std::memory_order_relaxed);
        g_sites[h->site].blocks.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_untracked.fetch_add((std::int64_t)size, std::memory_order_relaxed);
    }
    return h + 1;
}

void Give(void* p)
{
    if (!p)
        return;
    Header* h = static_cast<Header*>(p) - 1;
    if (h->site < kSites) {
        g_sites[h->site].bytes.fetch_sub((std::int64_t)h->size, std::memory_order_relaxed);
        g_sites[h->site].blocks.fetch_sub(1, std::memory_order_relaxed);
    } else {
        g_untracked.fetch_sub((std::int64_t)h->size, std::memory_order_relaxed);
    }
    h->magic = 0;
    std::free(h);
}
}  // namespace

void* operator new(std::size_t n)
{
    void* p = Take(n, (std::uintptr_t)__builtin_return_address(0));
    if (!p)
        throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n)
{
    void* p = Take(n, (std::uintptr_t)__builtin_return_address(0));
    if (!p)
        throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return Take(n, (std::uintptr_t)__builtin_return_address(0)); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return Take(n, (std::uintptr_t)__builtin_return_address(0)); }
void operator delete(void* p) noexcept { Give(p); }
void operator delete[](void* p) noexcept { Give(p); }
void operator delete(void* p, std::size_t) noexcept { Give(p); }
void operator delete[](void* p, std::size_t) noexcept { Give(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { Give(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { Give(p); }

namespace port {
void HeapProfileReport(int level)
{
    struct Row {
        std::uintptr_t pc;
        std::int64_t bytes, blocks;
    };
    static Row top[24];
    int n = 0;
    std::int64_t all = 0;
    for (std::size_t i = 0; i < kSites; i++) {
        const std::int64_t b = g_sites[i].bytes.load(std::memory_order_relaxed);
        if (g_sites[i].pc.load(std::memory_order_relaxed) == 0 || b <= 0)
            continue;
        all += b;
        Row r = {i, b, g_sites[i].blocks.load()};
        int at = n < 24 ? n++ : 24;
        if (at == 24) {
            if (b <= top[23].bytes)
                continue;
            at = 23;
        }
        top[at] = r;
        for (; at > 0 && top[at].bytes > top[at - 1].bytes; at--) {
            Row t = top[at];
            top[at] = top[at - 1];
            top[at - 1] = t;
        }
    }
    std::fprintf(stderr, "heapprof: level %d: %.1f MB held by new (%.1f MB at places not counted)\n", level, all / 1048576.0, g_untracked.load() / 1048576.0);
    for (int k = 0; k < n; k++)
    {
        const std::uintptr_t* c = g_sites[top[k].pc].chain;
        const std::uintptr_t b = ImageBase();
        std::fprintf(stderr, "heapprof: level %d: %.2f MB in %lld blocks: +0x%lX +0x%lX +0x%lX +0x%lX\n", level, top[k].bytes / 1048576.0, (long long)top[k].blocks,
                     (unsigned long)(c[0] - b), (unsigned long)(c[1] - b), (unsigned long)(c[2] - b), (unsigned long)(c[3] - b));
    }
}
}  // namespace port
