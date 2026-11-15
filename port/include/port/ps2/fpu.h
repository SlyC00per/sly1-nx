// SPDX-License-Identifier: AGPL-3.0-or-later
// The port's include/fpu.h (tools/gen.py puts it in place of the decompilation's): the same
// Fpu::Sqrt, computed as the header's "Computes:" line states: the EE's sqrt.s takes the root of
// the absolute value (never NaN), with VU0's float rules on the result.
#ifndef FPU_H
#define FPU_H

#include <cmath>

#include "common.h"
#include "port/vu0_impl.h"

struct Fpu {
    static f32 Sqrt(f32 x) { return port_vu0::Fix(std::sqrt(std::fabs(x))); }
};

#endif
