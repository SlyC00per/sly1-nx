/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* The port's include/mul_wide.h: a 32 by 32 bit unsigned multiply giving both halves, in C. */
#ifndef MUL_WIDE_H
#define MUL_WIDE_H
#define MUL_WIDE(high, low, a, b) \
    do { unsigned long long p_ = (unsigned long long)(unsigned)(a) * (unsigned)(b); \
         (low) = (unsigned)p_; (high) = (unsigned)(p_ >> 32); } while (0)
#endif
