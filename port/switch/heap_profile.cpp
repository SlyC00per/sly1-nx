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
    std::atomic<std::uintptr_t> pc{0};
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

std::uint32_t SiteFor(std::uintptr_t pc)
{
    std::size_t i = (pc * 0x9E3779B97F4A7C15ull) >> 50;
    for (std::size_t n = 0; n < kSites; n++, i = (i + 1) & (kSites - 1)) {
        std::uintptr_t was = g_sites[i].pc.load(std::memory_order_relaxed);
        if (was == pc)
            return (std::uint32_t)i;
        if (was == 0) {
            std::uintptr_t zero = 0;
            if (g_sites[i].pc.compare_exchange_strong(zero, pc) || zero == pc)
                return (std::uint32_t)i;
        }
    }
    return kSites;   // (full)
}

void* Take(std::size_t size, std::uintptr_t pc)
{
    Header* h = static_cast<Header*>(std::malloc(size + sizeof(Header)));
    if (!h)
        return nullptr;
    h->size = size;
    h->magic = kMagic;
    h->site = SiteFor(pc);
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
        Row r = {g_sites[i].pc.load(), b, g_sites[i].blocks.load()};
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
        std::fprintf(stderr, "heapprof: level %d: +0x%lX %.2f MB in %lld blocks\n", level, (unsigned long)(top[k].pc - ImageBase()), top[k].bytes / 1048576.0, (long long)top[k].blocks);
}
}  // namespace port
