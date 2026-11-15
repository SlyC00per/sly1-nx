// SPDX-License-Identifier: AGPL-3.0-or-later
// The PS2's variable-argument slots, built from a standard va_list.
//
// On the PS2 the game's variadic functions read their arguments as a run of 8-byte slots, the
// argument registers as the compiler saved them (__builtin_next_arg(last) - 8 * k): one argument
// per slot, a pointer or an int as its 32-bit value, a double as its 64 bits. The functions that
// do so (tools/patches.py lists them) keep their code: a patch fills such a run here from the
// arguments, with each argument's type taken from what the function's callers pass, and the rest
// reads it as before. A pointer goes in as its game address (port/ptr32.h), which is what the
// game's reads of a slot as a pointer expect after the pointer rewrite.
#ifndef PORT_VARARGS_H
#define PORT_VARARGS_H

#include <cstdarg>
#include <cstdint>
#include <cstring>

#include "port/ptr32.h"

namespace port {

using Slot = std::uint64_t;

inline Slot SlotPtr(const void* p) { return GameAddr(p); }
inline Slot SlotInt(int v) { return Slot(std::int64_t(v)); }
inline Slot SlotLong(long long v) { return Slot(v); }
inline Slot SlotDouble(double d)
{
    Slot s;
    std::memcpy(&s, &d, sizeof s);
    return s;
}

// The slots a printf-style format consumes, as the game's own formatter reads them
// (text_format.cpp, ParseFormatSpec and FormatConversionArg): `*` widths and precisions, the
// length modifiers h l ll L q, the game's vector and matrix modifiers v V m M (which make e f g
// read a pointer), and the conversions. Gives the number of slots written (at most max).
int FormatSlots(const char* fmt, std::va_list va, Slot* slots, int max);

}  // namespace port

#endif
