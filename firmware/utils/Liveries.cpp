#include "utils/Liveries.h"
#include <LittleFS.h>

namespace
{
    // "G-VPRD" and "gvprd" alike: letters and digits only, upper case.
    void key(const char *s, char *out, size_t len)
    {
        size_t n = 0;
        for (; *s && n + 1 < len; ++s)
            if (isalnum((unsigned char)*s)) out[n++] = (char)toupper((unsigned char)*s);
        out[n] = 0;
    }
}

bool Liveries::find(const char *reg, char *name, size_t len)
{
    if (!reg || !reg[0]) return false;
    char want[12];
    key(reg, want, sizeof(want));
    if (!want[0]) return false;
    File f = LittleFS.open(kPath, "r");
    if (!f) return false;
    char line[64];
    bool found = false;
    while (!found && f.available())
    {
        const size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '#') continue;
        char *sp = p;
        while (*sp && *sp != ' ' && *sp != '\t') ++sp;
        const char saved = *sp;
        *sp = 0;
        char have[12];
        key(p, have, sizeof(have));
        if (!strcmp(have, want))
        {
            char *nm = saved ? sp + 1 : sp;
            while (*nm == ' ' || *nm == '\t') ++nm;
            size_t l = strlen(nm);
            while (l && (nm[l - 1] == '\r' || nm[l - 1] == ' ')) nm[--l] = 0;
            strlcpy(name, l ? nm : "Special livery", len);
            found = true;
        }
    }
    f.close();
    return found;
}

void Liveries::read(String &out)
{
    out = "";
    File f = LittleFS.open(kPath, "r");
    if (!f) return;
    out.reserve(f.size() + 1);
    while (f.available()) out += (char)f.read();
    f.close();
}

bool Liveries::write(const String &text)
{
    if (text.length() > kMaxBytes) return false;
    File f = LittleFS.open(kPath, "w");
    if (!f) return false;
    const size_t n = f.print(text);
    f.close();
    return n == text.length();
}
