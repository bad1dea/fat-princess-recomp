// Forced into every game TU of an MSVC-ABI build (clang-cl). The UCRT has no clock_gettime, and the lifted code
// carries a few diagnostic prints that call it. Wall clock is enough for those.
#pragma once
#include <time.h>
#ifndef CLOCK_MONOTONIC
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
static inline int clock_gettime(int, struct timespec* ts) { return timespec_get(ts, TIME_UTC) ? 0 : -1; }
#endif

// The lifted mulhd/mulhdu use the x64 intrinsics _mul128/_umul128 under _MSC_VER. ARM64 has the high half only
// (__mulh/__umulh); the lift reads nothing but the high word.
#if defined(_M_ARM64)
#include <intrin.h>
static inline long long _mul128(long long a, long long b, long long* hi) {
    *hi = __mulh(a, b);
    return (long long)((unsigned long long)a * (unsigned long long)b);   /* low word, wrapping */
}
static inline unsigned long long _umul128(unsigned long long a, unsigned long long b, unsigned long long* hi) {
    *hi = __umulh(a, b);
    return a * b;
}
#endif
