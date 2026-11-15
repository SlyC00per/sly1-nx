// SPDX-License-Identifier: AGPL-3.0-or-later
// Game addresses and 32-bit game pointers (docs/DESIGN.md, section 4).
//
// The decompiled game keeps retail's structure layouts, in which a pointer is 4 bytes. So a
// pointer the game stores in memory (a structure field, a global, an array element) is a
// Ptr32<T>: a 32-bit game address. Pointers the game only holds in registers (locals,
// parameters, results) stay native. tools/gen.py writes the game's declarations that way.
//
// A game address is a 32-bit word with the PS2's meaning:
//   0                                    null
//   0x00000000 + x, x < kGameMemorySize  the program image at offset x (code, data, the
//                                        game's heap and stacks all live in the image)
//   0x20000000 + x, 0x30000000 + x       the same memory: the PS2's uncached aliases of RAM
//   0x70000000 + x, x < 0x4000           the scratchpad (a 16 KiB buffer of the port's)
//   anything else                        not memory here: a word carried in a pointer (the
//                                        game passes float bits as void*, 0x3F800000 is 1.0f),
//                                        or a hardware register's address
// Words that are not memory become tagged native pointers that give the same word back and fault
// when dereferenced, so a pointer can carry any 32-bit word and return it unchanged.
#ifndef PORT_PTR32_H
#define PORT_PTR32_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace port {

// Bytes of the program image game addresses can reach. The image (code, data, the game's heap
// and stacks) must lie within this many bytes of its base; ImageBase() checks it at start-up.
constexpr std::uint32_t kGameMemorySize = 0x04000000;
constexpr std::uint32_t kScratchpadAddr = 0x70000000;
constexpr std::uint32_t kScratchpadSize = 0x4000;
// The tag that marks a native pointer holding a word that is not memory. Not a valid address on
// any target (above the user half on AArch64 and x86-64), so a dereference faults.
constexpr std::uintptr_t kWordTag = std::uintptr_t(1) << 63;

// The start of the program image, the anchor of every game address. Found once and cached; the
// cache is a zero-initialised global, so it is valid even during static initialisation.
extern std::uintptr_t g_imageBase;
std::uintptr_t FindImageBase();
inline std::uintptr_t ImageBase() { return g_imageBase ? g_imageBase : FindImageBase(); }
// The port's scratchpad buffer (inside the image).
unsigned char* Scratchpad();

// A game address's native pointer, and back.
void* NativeSlow(std::uint32_t addr);
std::uint32_t GameAddrSlow(const void* p);

inline void* Native(std::uint32_t addr)
{
    if (addr - 1u < kGameMemorySize - 1u)
        return reinterpret_cast<void*>(ImageBase() + addr);
    return NativeSlow(addr);
}

inline std::uint32_t GameAddr(const void* p)
{
    std::uintptr_t d = reinterpret_cast<std::uintptr_t>(p) - ImageBase();
    if (d - 1u < std::uintptr_t(kGameMemorySize) - 1u)
        return static_cast<std::uint32_t>(d);
    return GameAddrSlow(p);
}

// The overloads tools/gen.py writes for a cast between a pointer and an integer.
template <class T> inline std::uint32_t GameAddrOf(T* p) { return GameAddr((const void*)p); }
template <class T> inline T* FromGameAddr(std::uint32_t addr) { return (T*)Native(addr); }

template <class T> class Ptr32 {
public:
    Ptr32() = default;
    Ptr32(std::nullptr_t) : addr_(0) {}
    Ptr32(T* p) : addr_(GameAddr((const void*)p)) {}
    template <class U, class = std::enable_if_t<!std::is_same<U, T>::value &&
                                                std::is_convertible<U*, T*>::value>>
    Ptr32(const Ptr32<U>& p) : Ptr32(static_cast<T*>(p.get())) {}

    T* get() const { return (T*)Native(addr_); }
    std::uint32_t addr() const { return addr_; }
    static Ptr32 FromAddr(std::uint32_t addr) { Ptr32 p; p.addr_ = addr; return p; }

    // To T* implicitly, as the game's code uses a pointer; to another pointer type only through
    // a cast, as C++ requires for a native pointer.
    operator T*() const { return get(); }
    template <class U, class = std::enable_if_t<!std::is_same<U, T>::value>>
    explicit operator U*() const { return (U*)get(); }

    T* operator->() const { return get(); }

    // The operators that need an lvalue; the others work through the conversion to T*.
    Ptr32& operator++() { return *this = get() + 1; }
    Ptr32& operator--() { return *this = get() - 1; }
    Ptr32 operator++(int) { Ptr32 old = *this; ++*this; return old; }
    Ptr32 operator--(int) { Ptr32 old = *this; --*this; return old; }
    Ptr32& operator+=(std::ptrdiff_t n) { return *this = get() + n; }
    Ptr32& operator-=(std::ptrdiff_t n) { return *this = get() - n; }

private:
    std::uint32_t addr_;
};

}  // namespace port

using port::Ptr32;

static_assert(sizeof(Ptr32<void>) == 4 && alignof(Ptr32<void>) == 4, "a game pointer is 4 bytes");
static_assert(std::is_trivially_copyable<Ptr32<int>>::value, "game pointers are copied as bytes");
static_assert(std::is_trivially_default_constructible<Ptr32<int>>::value, "and live in unions");

#endif
