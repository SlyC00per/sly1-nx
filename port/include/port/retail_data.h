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

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

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

// The audit of that room (a build with PORT_ROOM_AUDIT and ASan's recover mode): the bytes added
// past the local's own size are poisoned while it lives, so any code that reaches them, which
// retail's frame let reach a neighbour, is reported with its function and the variable.
#ifdef PORT_ROOM_AUDIT
}  // namespace port
#include <sanitizer/asan_interface.h>
namespace port {
struct RoomAudit {
    char* at;
    std::size_t size;
    RoomAudit(void* v, std::size_t own, std::size_t whole) : at((char*)v + own), size(whole - own)
    {
        // ASan poisons to the end of an 8-byte granule: up to the last granule end in the room
        std::uintptr_t end = ((std::uintptr_t)at + size) & ~std::uintptr_t(7);
        size = end > (std::uintptr_t)at ? end - (std::uintptr_t)at : 0;
        if (size)
            __asan_poison_memory_region(at, size);
    }
    ~RoomAudit()
    {
        if (size)
            __asan_unpoison_memory_region(at, size);
    }
};
#define PORT_ROOM_AUDIT_NAME2(l) port_room_audit_##l
#define PORT_ROOM_AUDIT_NAME(l) PORT_ROOM_AUDIT_NAME2(l)
#define PORT_ROOM_AUDIT(v, own) port::RoomAudit PORT_ROOM_AUDIT_NAME(__LINE__)((void*)&(v), own, sizeof(v))
#else
#define PORT_ROOM_AUDIT(v, own) ((void)0)
#endif

// The PS2 FPU's division (div.s): by zero, the largest float with the quotient's sign (0 / 0
// too), where IEEE gives infinity or NaN; tools/gen.py writes it for every float division of the
// game's code. An overflowing quotient already saturates (game threads round toward zero).
inline float FDiv(float a, float b)
{
    if (b == 0.0f)
        return std::signbit(a) != std::signbit(b) ? -3.4028235e38f : 3.4028235e38f;
    return a / b;
}
inline float& FDivAssign(float& a, float b) { return a = FDiv(a, b); }

// The level stream's state when a load closes it (instrumentation for --verify-levels): its
// unread decompressed bytes, compressed bytes not yet read, read but not yet unpacked, carried.
// The state of each level load's stream when it closes (milestone 1's byte check,
// tools/level_bytes.py): the last 16 closes, by count, with the texture block's state.
struct LevelStreamEnd {
    int remaining, bytesToRead, nextCount, carry, count, blockSize, blockLoad;
};
extern LevelStreamEnd g_levelStreamEnds[16];
extern std::atomic<int> g_levelStreamCloses;
void NoteLevelStreamEnd(int remaining, int bytesToRead, int nextCount, int carry);
// Under SLY1_STREAM_TRACE: the bytes of a game structure, as words, to compare platforms.
void TraceBytes(const char* what, const void* p, int n);

// A base address as the game's raw-offset macros take it (player.cpp's AT): a pointer as it is, a
// stored pointer, or a word holding a game address.
template <class T> inline void* AsAddress(T* p) { return (void*)p; }
template <class T> inline void* AsAddress(const Ptr32<T>& p) { return (void*)p.get(); }
template <class T, class = std::enable_if_t<std::is_integral<T>::value>>
inline void* AsAddress(T word) { return Native((std::uint32_t)word); }

// Preemption points (port/platform/kernel.cpp). tools/gen.py puts Preempt() in every loop test of
// the game's code: when an interrupt has readied a thread of higher priority, the running thread
// gives way there, as the PS2 preempts it. Spin() is an empty wait loop's body: the same, and a
// moment for the host's other threads.
extern std::atomic<bool> g_preemptPending;
void PreemptSlow();
#ifdef PORT_SENTINEL
// A software watchpoint (cmake -DSLY1_SENTINEL=ON): a zero word the kernel places right after its
// thread stacks, checked at every preemption point; a change is reported with the caller's frames.
extern volatile std::uint32_t g_stackEndSentinel[4];
[[noreturn]] void SentinelTripped();
#endif
inline void Preempt()
{
#ifdef PORT_SENTINEL
    if (g_stackEndSentinel[0] != 0)
        SentinelTripped();
#endif
    if (g_preemptPending.load(std::memory_order_relaxed))
        PreemptSlow();
}
void Spin();

// A local the game views through a larger type (its address cast to a Mat3* and written whole):
// the local with N bytes of room, its own type and fields as they were (tools/gen_pointers.py,
// roomy_cast). Retail's frame gave it that room in its neighbours and padding.
template <class T, std::size_t N> struct alignas(16) Room : T {
    char port_room[N - sizeof(T)];
    // Initialised as the local was: from a value of its type, from a braced list of its fields,
    // or not at all (zeroed here).
    template <class... A> Room(A... a) : T{a...}, port_room{} {}
    Room& operator=(const T& v)
    {
        static_cast<T&>(*this) = v;
        return *this;
    }
};

}  // namespace port

#endif
