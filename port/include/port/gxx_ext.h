// SPDX-License-Identifier: AGPL-3.0-or-later
// g++ 2.95's minimum and maximum operators, as tools/gen.py writes them: `a <? b` is
// PORT_MIN(a, b), `a >? b` PORT_MAX(a, b), `a >?= b` PORT_MAX_ASSIGN(a, b). g++ computes them
// as MIN_EXPR and MAX_EXPR in the operands' common type; the result here is the same value.
#ifndef PORT_GXX_EXT_H
#define PORT_GXX_EXT_H

#include <type_traits>

namespace port {

template <class A, class B> inline std::common_type_t<A, B> Min(A a, B b)
{
    using T = std::common_type_t<A, B>;
    return T(a) < T(b) ? T(a) : T(b);
}

template <class A, class B> inline std::common_type_t<A, B> Max(A a, B b)
{
    using T = std::common_type_t<A, B>;
    return T(a) > T(b) ? T(a) : T(b);
}

template <class A, class B> inline A& MinAssign(A& a, B b) { return a = A(Min(a, b)); }
template <class A, class B> inline A& MaxAssign(A& a, B b) { return a = A(Max(a, b)); }

}  // namespace port

#define PORT_MIN(a, b) (::port::Min((a), (b)))
#define PORT_MAX(a, b) (::port::Max((a), (b)))
#define PORT_MIN_ASSIGN(a, b) (::port::MinAssign((a), (b)))
#define PORT_MAX_ASSIGN(a, b) (::port::MaxAssign((a), (b)))

#endif
