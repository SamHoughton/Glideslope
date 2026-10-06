#include "utils/StageTrace.h"
#include "utils/TelnetLogger.h"
#include <esp_attr.h>
#include <esp_system.h>

namespace
{
    constexpr uint32_t kMagic = 0x5EA6E001;
    RTC_NOINIT_ATTR uint32_t s_magic;
    RTC_NOINIT_ATTR uint8_t  s_stage[2];
    RTC_NOINIT_ATTR uint8_t  s_detail[2];

    const char *displayStage(uint8_t s)
    {
        switch (s)
        {
            case StageTrace::DispWaitLock: return "waiting for the display lock";
            case StageTrace::DispRender:   return "drawing a frame";
            case StageTrace::DispPresent:  return "sending pixels to the panel";
            case StageTrace::DispSleep:    return "idle between frames";
            default:                       return "unknown";
        }
    }

    const char *webStage(uint8_t s)
    {
        switch (s)
        {
            case StageTrace::WebAccept:      return "waiting for a connection";
            case StageTrace::WebReadRequest: return "reading a request";
            case StageTrace::WebHandle:      return "answering a request";
            case StageTrace::WebIdle:        return "idle";
            default:                         return "unknown";
        }
    }
}

void StageTrace::mark(Task t, uint8_t stage, uint8_t detail)
{
    s_magic     = kMagic;
    s_stage[t]  = stage;
    s_detail[t] = detail;
}

static char s_lastReset[192] = "unknown";

void StageTrace::reportAtBoot()
{
    const esp_reset_reason_t r = esp_reset_reason();
    const char *why = r == ESP_RST_POWERON  ? "power on"
                    : r == ESP_RST_SW       ? "restart"
                    : r == ESP_RST_TASK_WDT ? "watchdog (a task hung)"
                    : r == ESP_RST_INT_WDT  ? "interrupt watchdog"
                    : r == ESP_RST_PANIC    ? "crash"
                    : r == ESP_RST_BROWNOUT ? "brownout (power dip)"
                                            : "other (e.g. flashing)";
    const bool abnormal = r == ESP_RST_TASK_WDT || r == ESP_RST_INT_WDT ||
                          r == ESP_RST_WDT || r == ESP_RST_PANIC;
    if (abnormal && s_magic == kMagic)
        snprintf(s_lastReset, sizeof(s_lastReset), "%s; display was %s, web was %s (route %u)",
                 why, displayStage(s_stage[Display]), webStage(s_stage[Web]), (unsigned)s_detail[Web]);
    else
        snprintf(s_lastReset, sizeof(s_lastReset), "%s", why);
    Log.printf("Boot: last reset: %s\n", s_lastReset);
    s_magic = 0;
}

const char *StageTrace::lastReset() { return s_lastReset; }
