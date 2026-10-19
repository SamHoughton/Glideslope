#pragma once
// Just enough of the Arduino API to build the board's logic (approach model,
// trackers, parsers) on a desktop for the unit tests (pio test -e native).
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <type_traits>

#ifndef PROGMEM
#define PROGMEM   // flash tables are ordinary arrays on the host
#endif
#include <stdarg.h>
#include <time.h>
#include <string>
#include <algorithm>

typedef uint8_t byte;

// A clock the tests drive.
extern unsigned long g_fakeMillis;
inline unsigned long millis() { return g_fakeMillis; }
inline void delay(unsigned long ms) { g_fakeMillis += ms; }
inline uint32_t esp_random() { return (uint32_t)rand() * 2654435761u; }

// By value: decltype(a < b ? a : b) is a reference to a parameter, which
// dangled (garbage under optimisation).
template <class A, class B> inline typename std::common_type<A, B>::type min(A a, B b) { return a < b ? a : b; }
template <class A, class B> inline typename std::common_type<A, B>::type max(A a, B b) { return a < b ? b : a; }

#if !defined(__GLIBC__) || __GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38)
inline size_t strlcpy(char *dst, const char *src, size_t n)
{
    const size_t len = strlen(src);
    if (n) { const size_t c = len < n - 1 ? len : n - 1; memcpy(dst, src, c); dst[c] = '\0'; }
    return len;
}
inline size_t strlcat(char *dst, const char *src, size_t n)
{
    const size_t d = strnlen(dst, n);
    if (d == n) return n + strlen(src);
    return d + strlcpy(dst + d, src, n - d);
}
#endif

class String
{
public:
    String() = default;
    String(const char *s) : _s(s ? s : "") {}
    String(const std::string &s) : _s(s) {}
    String(char c) : _s(1, c) {}
    String(int v) : _s(std::to_string(v)) {}
    String(unsigned v) : _s(std::to_string(v)) {}
    String(long v) : _s(std::to_string(v)) {}
    String(unsigned long v) : _s(std::to_string(v)) {}
    String(double v, int dec = 2) { char b[48]; snprintf(b, sizeof(b), "%.*f", dec, v); _s = b; }

    const char *c_str() const { return _s.c_str(); }
    unsigned length() const { return (unsigned)_s.size(); }
    bool isEmpty() const { return _s.empty(); }
    void reserve(unsigned n) { _s.reserve(n); }
    char operator[](unsigned i) const { return i < _s.size() ? _s[i] : 0; }
    char &operator[](unsigned i) { return _s[i]; }
    char charAt(unsigned i) const { return (*this)[i]; }

    bool concat(const char *s) { _s += s ? s : ""; return true; }
    bool concat(const char *s, unsigned n) { _s.append(s, n); return true; }
    bool concat(char c) { _s += c; return true; }
    bool concat(const String &s) { _s += s._s; return true; }
    String &operator+=(const String &s) { _s += s._s; return *this; }
    String &operator+=(const char *s) { _s += s; return *this; }
    String &operator+=(char c) { _s += c; return *this; }
    friend String operator+(const String &a, const String &b) { return String(a._s + b._s); }
    friend String operator+(const String &a, const char *b) { return String(a._s + b); }
    friend String operator+(const char *a, const String &b) { return String(std::string(a) + b._s); }
    bool operator==(const String &o) const { return _s == o._s; }
    bool operator==(const char *o) const { return _s == (o ? o : ""); }
    bool operator!=(const String &o) const { return _s != o._s; }
    bool operator!=(const char *o) const { return !(*this == o); }
    bool operator<(const String &o) const { return _s < o._s; }
    bool equals(const String &o) const { return _s == o._s; }
    bool equalsIgnoreCase(const String &o) const { return strcasecmp(c_str(), o.c_str()) == 0; }

    int indexOf(char c, unsigned from = 0) const { auto p = _s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
    int indexOf(const String &s, unsigned from = 0) const { auto p = _s.find(s._s, from); return p == std::string::npos ? -1 : (int)p; }
    int lastIndexOf(char c) const { auto p = _s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
    String substring(unsigned from) const { return from >= _s.size() ? String() : String(_s.substr(from)); }
    String substring(unsigned from, unsigned to) const
    {
        if (from > to) std::swap(from, to);
        if (from >= _s.size()) return String();
        return String(_s.substr(from, std::min<size_t>(to, _s.size()) - from));
    }
    bool startsWith(const String &p) const { return _s.compare(0, p._s.size(), p._s) == 0; }
    bool endsWith(const String &p) const { return _s.size() >= p._s.size() && _s.compare(_s.size() - p._s.size(), p._s.size(), p._s) == 0; }
    void trim()
    {
        size_t a = 0, b = _s.size();
        while (a < b && isspace((unsigned char)_s[a])) ++a;
        while (b > a && isspace((unsigned char)_s[b - 1])) --b;
        _s = _s.substr(a, b - a);
    }
    void toUpperCase() { for (auto &c : _s) c = (char)toupper((unsigned char)c); }
    void toLowerCase() { for (auto &c : _s) c = (char)tolower((unsigned char)c); }
    void replace(const String &from, const String &to)
    {
        if (from._s.empty()) return;
        for (size_t p = 0; (p = _s.find(from._s, p)) != std::string::npos; p += to._s.size()) _s.replace(p, from._s.size(), to._s);
    }
    long toInt() const { return atol(_s.c_str()); }
    double toDouble() const { return atof(_s.c_str()); }
    float toFloat() const { return (float)atof(_s.c_str()); }

    // ArduinoJson's String support
    explicit operator bool() const { return true; }

private:
    std::string _s;
};

class Print
{
public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t) { return 1; }
    virtual size_t write(const uint8_t *buf, size_t n) { for (size_t i = 0; i < n; ++i) write(buf[i]); return n; }
};

class Printable
{
public:
    virtual ~Printable() = default;
    virtual size_t printTo(Print &p) const = 0;
};

class Stream : public Print
{
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    void setTimeout(unsigned long) {}
    size_t readBytes(char *buf, size_t n)
    {
        size_t i = 0;
        for (; i < n; ++i) { const int c = read(); if (c < 0) break; buf[i] = (char)c; }
        return i;
    }
    size_t readBytes(uint8_t *buf, size_t n) { return readBytes((char *)buf, n); }
    bool find(const char *target)
    {
        const size_t n = strlen(target);
        size_t m = 0;
        for (int c; (c = read()) >= 0;)
        {
            m = (c == target[m]) ? m + 1 : (c == target[0] ? 1 : 0);
            if (m == n) return true;
        }
        return false;
    }
    bool find(char *target) { return find((const char *)target); }
    String readStringUntil(char term)
    {
        String s;
        for (int c; (c = read()) >= 0 && c != term;) s += (char)c;
        return s;
    }
};

// A Stream over a string (for parser tests).
class StringStream : public Stream
{
public:
    explicit StringStream(std::string s) : _s(std::move(s)) {}
    int available() override { return (int)(_s.size() - _p); }
    int read() override { return _p < _s.size() ? (unsigned char)_s[_p++] : -1; }
    int peek() override { return _p < _s.size() ? (unsigned char)_s[_p] : -1; }

private:
    std::string _s;
    size_t _p = 0;
};
