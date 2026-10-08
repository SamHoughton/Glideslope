#pragma once
/*
Purpose: Phone notifications through ntfy (https://ntfy.sh: free, no account,
apps for iOS and Android). The board posts to a topic; anyone subscribed to
that topic in the ntfy app gets the alert. Topics are public by name, so the
web page suggests a long random one.

  POST http://ntfy.sh/<topic>    Title, Tags and Priority headers, text body

Plain HTTP to ntfy.sh (no TLS handshake, so no heap spike); a full https URL
for a self-hosted server works too, when there is room for TLS.

Events (each can be switched off on the web page): emergency squawks, rare
spots, go-arounds, runway changes and busy holding stacks. post() is safe
from any task and only queues (3 deep); the main loop sends one at a time
between fetches. Each kind is sent at most once every 2 minutes, emergencies
excepted.
*/
#include <Arduino.h>

namespace Notify
{
    enum Kind : uint8_t
    {
        Emergency = 1,
        Rare      = 2,
        GoAround  = 4,
        Runway    = 8,
        Holding   = 16,
        Daily     = 32,    // the day's round-up at 22:30
        Test      = 128,   // always allowed when a topic is set
    };

    // priority: ntfy's 1 (min) .. 5 (max); tags: ntfy emoji short codes, comma separated.
    void post(Kind kind, uint8_t priority, const char *tags, const char *title, const char *fmt, ...)
        __attribute__((format(printf, 5, 6)));

    // Main loop: send one queued notification, if any (blocks up to ~5 s).
    void loop();
}
