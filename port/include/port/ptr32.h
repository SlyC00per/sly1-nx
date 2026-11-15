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
//   0x20000000 + x                       the same memory: the PS2's uncached alias of RAM (the game
//                                        writes its deferred requests through it; 0x30000000, the
//                                        accelerated alias, it never uses, so those words are kept)
//   0x70000000 + x, x < 0x4000           the scratchpad (a 16 KiB buffer of the port's)
//   0x10000000 + x, x < 0x10000;         the hardware registers the game uses: plain memory of the
//   0x12000000 + x, x < 0x2000           port's (port/platform/game_memory.cpp)
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

// The most the program image may span: game addresses below this are image offsets. The image's
// real extent (ImageSize(), measured at start-up) decides what is game memory: a pointer is an
// image pointer only inside it, and a word only becomes one when it is within it.
constexpr std::uint32_t kGameMemorySize = 0x08000000;
constexpr std::uint32_t kScratchpadAddr = 0x70000000;
constexpr std::uint32_t kScratchpadSize = 0x4000;
// The tag that marks a native pointer holding a word that is not memory. Not a valid address on
// any target (above the user half on AArch64 and x86-64), so a dereference faults.
constexpr std::uintptr_t kWordTag = std::uintptr_t(1) << 63;

// The start of the program image, the anchor of every game address. Found once and cached; the
// cache is a zero-initialised global, so it is valid even during static initialisation.
extern std::uintptr_t g_imageBase;
extern std::uintptr_t g_imageSize;
std::uintptr_t FindImageBase();
inline std::uintptr_t ImageBase() { return g_imageBase ? g_imageBase : FindImageBase(); }
inline std::uintptr_t ImageSize() { return g_imageBase ? g_imageSize : (FindImageBase(), g_imageSize); }
// The port's scratchpad buffer (inside the image).
unsigned char* Scratchpad();

// A game address's native pointer, and back.
void* NativeSlow(std::uint32_t addr);
std::uint32_t GameAddrSlow(const void* p);

inline void* Native(std::uint32_t addr)
{
    if (addr - 1u < ImageSize() - 1u)
        return reinterpret_cast<void*>(ImageBase() + addr);
    return NativeSlow(addr);
}

inline std::uint32_t GameAddr(const void* p)
{
    std::uintptr_t d = reinterpret_cast<std::uintptr_t>(p) - ImageBase();
    if (d - 1u < ImageSize() - 1u)
        return static_cast<std::uint32_t>(d);
    return GameAddrSlow(p);
}

// A word the game passes where a declaration says pointer and another says integer (a semaphore's
// id, both through one C-linkage function): a plain number arrives as itself, below the image; a
// word the pointer rewrite converted arrives as its native pointer. Either gives the word back.
inline std::uint32_t WordArg(const void* p)
{
    std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
    return v < ImageBase() ? static_cast<std::uint32_t>(v) : GameAddr(p);
}

// The overloads tools/gen.py writes for a cast between a pointer and an integer.
template <class T> inline std::uint32_t GameAddrOf(T* p) { return GameAddr((const void*)p); }
template <class T> inline T* FromGameAddr(std::uint32_t addr) { return (T*)Native(addr); }

// A stored pointer. Plain old data in C++03's sense: no constructors, one public member. That is
// what keeps retail's layouts on every compiler: a class with constructors would make the game's
// structures non-POD, and today's ABI (GCC more than Clang) then reuses their tail padding, which
// g++ 2.95 never did; GCC also refuses a member with constructors in an anonymous union or
// structure, which the game has by the hundred. So a Ptr32 is assigned (from a T*, a Ptr32 of a
// convertible type, or 0) and initialised from port::P32(p), which tools/gen.py writes around a
// stored pointer's initialiser.
template <class T> struct Ptr32 {
    std::uint32_t addr_;

    T* get() const { return (T*)Native(addr_); }
    std::uint32_t addr() const { return addr_; }
    static Ptr32 FromAddr(std::uint32_t addr) { Ptr32 p; p.addr_ = addr; return p; }

    Ptr32& operator=(T* p) { addr_ = GameAddr((const void*)p); return *this; }
    template <class U, class = std::enable_if_t<!std::is_same<U, T>::value &&
                                                std::is_convertible<U*, T*>::value>>
    Ptr32& operator=(const Ptr32<U>& p) { return *this = static_cast<T*>(p.get()); }

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
};

// A game address on its way into a Ptr32 of any type: port::P32's result. It initialises a
// stored pointer (`Ptr32<T> g = port::P32(&x);`, `{ port::P32(&x), ... }` in a table) through
// its conversion, as Ptr32 has no constructors.
struct GameWord {
    std::uint32_t addr;
    template <class U> operator Ptr32<U>() const { return Ptr32<U>::FromAddr(addr); }
};

template <class T> inline GameWord P32(T* p) { return GameWord{GameAddr((const void*)p)}; }
template <class T> inline GameWord P32(const Ptr32<T>& p) { return GameWord{p.addr_}; }
inline GameWord P32(std::nullptr_t) { return GameWord{0}; }

// A stored pointer made an integer: its game address, unchanged.
template <class T> inline std::uint32_t GameAddrOf(const Ptr32<T>& p) { return p.addr(); }

}  // namespace port

using port::Ptr32;

static_assert(sizeof(Ptr32<void>) == 4 && alignof(Ptr32<void>) == 4, "a game pointer is 4 bytes");
static_assert(std::is_trivial<Ptr32<int>>::value && std::is_standard_layout<Ptr32<int>>::value,
              "a game pointer is plain old data");
static_assert(std::is_aggregate<Ptr32<int>>::value, "with no constructors");

#endif
