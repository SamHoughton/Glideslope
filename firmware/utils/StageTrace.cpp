#include "utils/StageTrace.h"
#include "utils/TelnetLogger.h"
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_core_dump.h>
#include <esp_ota_ops.h>

namespace
{
    constexpr uint32_t kMagic = 0x5EA6E001;
    RTC_NOINIT_ATTR uint32_t s_magic;
    RTC_NOINIT_ATTR uint8_t  s_stage[3];
    RTC_NOINIT_ATTR uint8_t  s_detail[3];

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

    const char *mainStage(uint8_t s)
    {
        switch (s)
        {
            case StageTrace::MainIdle:    return "idle";
            case StageTrace::MainFetch:   return "fetching positions and routes";
            case StageTrace::MainStack:   return "sampling a holding stack";
            case StageTrace::MainWeather: return "fetching the weather";
            case StageTrace::MainNotify:  return "sending a notification";
            case StageTrace::MainStats:   return "saving stats";
            case StageTrace::MainTraffic: return "processing the traffic";
            default:                      return "unknown";
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
        snprintf(s_lastReset, sizeof(s_lastReset), "%s; main loop was %s, display was %s, web was %s (route %u)",
                 why, mainStage(s_stage[Main]), displayStage(s_stage[Display]), webStage(s_stage[Web]),
                 (unsigned)s_detail[Web]);
    else
        snprintf(s_lastReset, sizeof(s_lastReset), "%s", why);
    Log.printf("Boot: last reset: %s\n", s_lastReset);
    static char dump[256];
    if (coreDump(dump, sizeof(dump))) Log.printf("Boot: core dump saved: %s\n", dump);
    s_magic = 0;
}

const char *StageTrace::lastReset() { return s_lastReset; }

bool StageTrace::coreDump(char *out, size_t len)
{
    if (esp_core_dump_image_check() != ESP_OK) return false;
    esp_core_dump_summary_t *s = (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
    if (!s) return false;
    const bool ok = esp_core_dump_get_summary(s) == ESP_OK;
    if (ok)
    {
        int n = snprintf(out, len, "task %.16s, pc 0x%08x, cause %u, vaddr 0x%08x, backtrace%s:",
                         s->exc_task, (unsigned)s->exc_pc, (unsigned)s->ex_info.exc_cause,
                         (unsigned)s->ex_info.exc_vaddr, s->exc_bt_info.corrupted ? " (corrupted)" : "");
        for (uint32_t i = 0; i < s->exc_bt_info.depth && i < 16 && n > 0 && (size_t)n < len; ++i)
            n += snprintf(out + n, len - n, " 0x%08x", (unsigned)s->exc_bt_info.bt[i]);
    }
    free(s);
    return ok;
}

bool StageTrace::coreDumpFromThisBuild(char *elf, size_t len)
{
    if (len) elf[0] = '\0';
    if (esp_core_dump_image_check() != ESP_OK) return false;
    esp_core_dump_summary_t *s = (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
    if (!s) return false;
    bool same = false;
    if (esp_core_dump_get_summary(s) == ESP_OK)
    {
        char running[sizeof(s->app_elf_sha256)] = "";
        esp_ota_get_app_elf_sha256(running, sizeof(running));
        const char *dumped = (const char *)s->app_elf_sha256;
        strlcpy(elf, dumped, len);
        same = running[0] && strncmp(running, dumped, sizeof(running) - 1) == 0;
    }
    free(s);
    return same;
}

void StageTrace::eraseCoreDump()
{
    esp_core_dump_image_erase();
}
