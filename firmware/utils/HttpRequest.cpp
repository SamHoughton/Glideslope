#include "utils/HttpRequest.h"

namespace HttpRequest
{
bool parseLine(const String &raw, Line &out)
{
    const int eol = raw.indexOf("\r\n");
    const String line = eol >= 0 ? raw.substring(0, eol) : raw;
    const int sp1 = line.indexOf(' ');
    if (sp1 <= 0) return false;
    const int sp2 = line.indexOf(' ', sp1 + 1);
    if (sp2 < 0 || sp2 == sp1 + 1 || sp2 + 1 >= (int)line.length()) return false;
    const String full = line.substring(sp1 + 1, sp2);
    if (full[0] != '/') return false;
    out.method = line.substring(0, sp1);
    const int q = full.indexOf('?');
    out.path  = q >= 0 ? full.substring(0, q) : full;
    out.query = q >= 0 ? full.substring(q + 1) : String();
    return true;
}

String header(const String &raw, const char *name)
{
    const int n = strlen(name);
    if (!n) return String();
    for (int i = raw.indexOf("\r\n"); i >= 0; i = raw.indexOf("\r\n", i + 2))
    {
        const int start = i + 2;
        if (start + n >= (int)raw.length()) break;
        if (strncasecmp(raw.c_str() + start, name, n) == 0 && raw[start + n] == ':')
        {
            const int end = raw.indexOf("\r\n", start);
            String v = raw.substring(start + n + 1, end < 0 ? raw.length() : end);
            v.trim();
            return v;
        }
    }
    return String();
}

long contentLength(const String &raw)
{
    const String v = header(raw, "Content-Length");
    if (!v.length() || v.length() > 9) return -1;   // > 9 digits: over 100 MB anyway
    for (unsigned i = 0; i < v.length(); ++i)
        if (v[i] < '0' || v[i] > '9') return -1;
    const long n = v.toInt();
    return n <= 100L * 1024 * 1024 ? n : -1;
}

String qparam(const String &query, const char *key)
{
    const String k = String(key) + "=";
    for (int pos = query.indexOf(k); pos >= 0; pos = query.indexOf(k, pos + 1))
    {
        if (pos > 0 && query[pos - 1] != '&') continue;   // inside another key
        const int start = pos + k.length();
        const int end = query.indexOf('&', start);
        return end < 0 ? query.substring(start) : query.substring(start, end);
    }
    return String();
}
}
