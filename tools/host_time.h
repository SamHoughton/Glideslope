// Forced into the host renderer's build (tools/host_build.py render/golden):
// every time() call in the firmware reads a fixed moment, 18 Oct 2026 19:15
// UTC (20:15 in London), so rendered screens are the same on every run and
// can be compared with the reference images in firmware/test/golden.
#pragma once
#include <time.h>
#ifdef __cplusplus
inline time_t gs_fixed_time(time_t *t)
{
    const time_t v = 1792350900;
    if (t) *t = v;
    return v;
}
#define time(t) gs_fixed_time(t)
#endif
