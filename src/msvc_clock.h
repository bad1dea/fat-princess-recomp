// Forced into every game TU of an MSVC-ABI build (clang-cl). The UCRT has no clock_gettime, and the lifted code
// carries a few diagnostic prints that call it. Wall clock is enough for those.
#pragma once
#include <time.h>
#ifndef CLOCK_MONOTONIC
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
static inline int clock_gettime(int, struct timespec* ts) { return timespec_get(ts, TIME_UTC) ? 0 : -1; }
#endif
