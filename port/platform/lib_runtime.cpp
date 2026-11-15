// SPDX-License-Identifier: AGPL-3.0-or-later
// The platform layer: the toolchain's runtime and the C library functions the game calls
// (sly1-decomp src/lib, src/libm; their roles in config/names/lib and libm), on the host's own.
//
// The soft-float routines of the PS2's runtime implement IEEE double arithmetic, which the host's
// doubles are; the conversions follow the roles the name tables give. The math library's
// functions are the host's single-precision ones: their last bits may differ from retail's
// (newlib's kernels), which is listed in docs/DESIGN.md as a measured-later difference.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common.h"

extern "C" {

// --- 64-bit integer and double helpers (libgcc and soft-float, called by name)

f32 ConvertS64ToFloat(s64 x) { return (f32)x; }
u64 DivideUnsigned64(u64 num, u64 den) { return den ? num / den : 0; }
s64 RemainderUnsigned64(u64 num, u64 den) { return (s64)(den ? num % den : num); }
double AddDoubles(double a, double b) { return a + b; }
double SubtractDoubles(double a, double b) { return a - b; }
double MultiplyDoubles(double a, double b) { return a * b; }

// The order of a and b: negative, zero or positive; an unordered pair (a NaN) compares greater,
// as libgcc's __cmpdf2 does.
s32 CompareDoubles(double a, double b)
{
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return a == b ? 0 : 1;
}

double ConvertS32ToDouble(s32 a) { return (double)a; }

// 0 for NaN and zero, saturating at the limits.
s32 ConvertDoubleToS32(double a)
{
    if (a != a)
        return 0;
    if (a >= 2147483647.0)
        return 0x7FFFFFFF;
    if (a <= -2147483648.0)
        return (s32)0x80000000;
    return (s32)a;
}

f32 ConvertDoubleToFloat(double x) { return (f32)x; }

// A float to an unsigned 32-bit integer, as libgcc's __fixunssfsi: from 2^31 up through the
// signed conversion of the value less 2^31.
s32 ConvertFloatToU32(f32 value)
{
    if (value >= 2147483648.0f)
        return (s32)((u32)(s32)(value - 2147483648.0f) + 0x80000000u);
    return (s32)value;
}

double ConvertFloatToDouble(f32 x) { return (double)x; }

// A call of a method a class leaves abstract: retail runs the terminate hook.
void HandleAbstractCall()
{
    std::fprintf(stderr, "port: an abstract method was called (HandleAbstractCall)\n");
    std::abort();
}

// The library's buffered printf, to the log.
void PrintBuffered(const char* fmt, ...)
{
    std::va_list va;
    va_start(va, fmt);
    std::vfprintf(stderr, fmt, va);
    va_end(va);
}

// --- the hand-written memory and string routines

void* CopyBytes(void* dst, void* src, s32 n) { return std::memmove(dst, src, (size_t)n); }
void* FillMemory(void* p, s32 value, s32 size) { return std::memset(p, value, (size_t)size); }

s32 CompareStrings(const char* a, const char* b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (s32)(u8)*a - (s32)(u8)*b;
}

// --- the math library

f32 Arctangent(f32 x) { return std::atan(x); }
f32 Cosine(f32 x) { return std::cos(x); }
f32 RoundDown(f32 x) { return std::floor(x); }
f32 Sine(f32 x) { return std::sin(x); }
f32 Tangent(f32 x) { return std::tan(x); }
f32 Arccosine(f32 x) { return std::acos(x); }
f32 Arcsine(f32 x) { return std::asin(x); }
f32 ArctangentOfRatio(f32 y, f32 x) { return std::atan2(y, x); }
f32 FloatRemainder(f32 x, f32 y) { return std::fmod(x, y); }
f32 Logarithm(f32 x) { return std::log(x); }
f32 LogarithmBase10(f32 x) { return std::log10(x); }
f32 Power(f32 x, f32 y) { return std::pow(x, y); }
f32 SquareRoot(f32 x) { return std::sqrt(x); }

// --- the linker's leftovers that a table still names: no role, no code

void func_00135E20() {}
void func_00135E40() {}

}  // extern "C"
