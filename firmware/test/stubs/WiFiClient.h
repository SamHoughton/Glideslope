#pragma once
// A web client for the native tests: whatever is sent to it is kept in
// `sent`, so CSV and JSON writers can be checked.
#include <Arduino.h>
#include <cstdarg>
#include <string>

class WiFiClient
{
public:
    std::string sent;
    bool connected() const { return true; }
    size_t print(const char *s) { sent += s; return strlen(s); }
    size_t print(const String &s) { return print(s.c_str()); }
    int printf(const char *fmt, ...)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        sent += buf;
        return n;
    }
    size_t write(const uint8_t *b, size_t n) { sent.append((const char *)b, n); return n; }
};
