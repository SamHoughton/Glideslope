#pragma once
/*
Purpose: Hang diagnostics. The display and web tasks record what they are
doing in RTC memory, which survives a watchdog or crash reset. After such a
reset, reportAtBoot() logs where each task was, so a hang can be located
without a debugger.
*/
#include <Arduino.h>

namespace StageTrace
{
    enum Task : uint8_t { Display = 0, Web = 1 };

    // Display stages
    enum : uint8_t { DispWaitLock = 1, DispRender = 2, DispPresent = 3, DispSleep = 4 };
    // Web stages (the handler stage also records which route)
    enum : uint8_t { WebAccept = 1, WebReadRequest = 2, WebHandle = 3, WebIdle = 4 };

    void mark(Task t, uint8_t stage, uint8_t detail = 0);

    // Call early in setup(), after the logger is ready.
    void reportAtBoot();

    // One-line description of the last reset (reason, and where the tasks
    // were if it was a watchdog or crash). Valid after reportAtBoot().
    const char *lastReset();
}
