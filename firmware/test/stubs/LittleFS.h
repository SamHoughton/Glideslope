#pragma once
// No filesystem in the native tests: every open fails, so code that loads
// saved state starts clean and saving reports failure.
#include <Arduino.h>

class File
{
public:
    explicit operator bool() const { return false; }
    size_t read(uint8_t *, size_t) { return 0; }
    size_t write(const uint8_t *, size_t) { return 0; }
    String readStringUntil(char) { return String(); }
    void close() {}
};

struct LittleFSStub
{
    File open(const char *, const char * = "r") { return File(); }
    File open(const String &p, const char *m = "r") { return open(p.c_str(), m); }
    bool exists(const char *) { return false; }
    bool remove(const char *) { return false; }
    bool rename(const char *, const char *) { return false; }
};
inline LittleFSStub LittleFS;
