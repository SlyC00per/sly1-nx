// SPDX-License-Identifier: AGPL-3.0-or-later
// The EE's and VU0's float rules on the game's threads: rounding toward zero, denormals flushed
// to zero (sly1-decomp include/vu0.h's header comment). Each game thread sets them when it starts.
#include "port/system.h"

#include <cfenv>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace port {

void SetGameFloatMode()
{
    std::fesetround(FE_TOWARDZERO);
#if defined(__aarch64__)
    std::uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= std::uint64_t(1) << 24;   // FZ: denormals flushed to zero
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() | 0x8040);   // FTZ and DAZ
#endif
}

}  // namespace port
