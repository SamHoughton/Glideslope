#pragma once
// The board's log, for the native tests: printed to stdout when GS_TEST_LOG is set.
#include <Arduino.h>

struct TestLog
{
    bool on = getenv("GS_TEST_LOG") != nullptr;
    void printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        if (!on) return;
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    void println(const char *s = "") { if (on) puts(s); }
    void println(const String &s) { println(s.c_str()); }
    void print(const char *s) { if (on) fputs(s, stdout); }
};
inline TestLog Log;
