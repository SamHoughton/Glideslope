#include "utils/RouteBook.h"
#include "utils/TelnetLogger.h"
#include <LittleFS.h>

namespace
{
    constexpr const char *kFile = "/routes.txt";
    constexpr const char *kOld  = "/routes.old";
    constexpr size_t      kMaxBytes = 96 * 1024;

    bool validIcao(const char *s)
    {
        if (!s || strlen(s) != 4) return false;
        for (int i = 0; i < 4; ++i)
            if (!isalnum((unsigned char)s[i])) return false;
        return true;
    }

    // The last line for this call sign in one file.
    bool scan(const char *path, const char *callsign, char *origin, char *dest)
    {
        File f = LittleFS.open(path, "r");
        if (!f) return false;
        const size_t n = strlen(callsign);
        bool found = false;
        char line[32];
        while (f.available())
        {
            const size_t len = f.readBytesUntil('\n', line, sizeof(line) - 1);
            line[len] = 0;
            if (len == n + 10 && !strncmp(line, callsign, n) && line[n] == ' ' && line[n + 5] == ' ')
            {
                memcpy(origin, line + n + 1, 4); origin[4] = 0;
                memcpy(dest,   line + n + 6, 4); dest[4] = 0;
                found = true;
            }
        }
        f.close();
        return found;
    }
}

bool RouteBook::find(const char *callsign, char *origin, char *dest)
{
    if (!callsign || !callsign[0] || strlen(callsign) > 8) return false;
    return scan(kFile, callsign, origin, dest) || scan(kOld, callsign, origin, dest);
}

void RouteBook::remember(const char *callsign, const char *origin, const char *dest)
{
    if (!callsign || !callsign[0] || strlen(callsign) > 8 || strchr(callsign, ' ')) return;
    if (!validIcao(origin) || !validIcao(dest)) return;
    char o[5], d[5];
    if (find(callsign, o, d) && !strcmp(o, origin) && !strcmp(d, dest)) return;   // known already
    {
        File f = LittleFS.open(kFile, "r");
        const size_t size = f ? f.size() : 0;
        if (f) f.close();
        if (size > kMaxBytes)
        {
            LittleFS.remove(kOld);
            LittleFS.rename(kFile, kOld);
        }
    }
    File f = LittleFS.open(kFile, "a");
    if (!f) return;
    f.printf("%s %s %s\n", callsign, origin, dest);
    f.close();
    Log.printf("RouteBook: learned %s %s>%s\n", callsign, origin, dest);
}
