#pragma once
/*
Purpose: Find what pushes the heap lowest. Call heapCheckpoint("step") after
each memory-hungry step (TLS fetches, JSON parsing, web requests); when the
all-time minimum free heap has dropped since the last checkpoint, the step
just finished is logged as the culprit.
*/
#include <Arduino.h>
#include "utils/TelnetLogger.h"

inline void heapCheckpoint(const char *stage)
{
    static uint32_t low = UINT32_MAX;
    const uint32_t m = ESP.getMinFreeHeap();
    if (m >= low) return;
    if (low != UINT32_MAX)
        Log.printf("Heap: new low %u after %s (free now %u, largest block %u)\n",
                   (unsigned)m, stage, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    low = m;
}
