#include "utils/TlsArena.h"
#include "utils/TelnetLogger.h"
extern "C" {
#include <esp_mem.h>   // the default allocator mbedTLS used before (C linkage: first)
}
#include <mbedtls/platform.h>

namespace TlsArena
{
namespace
{
// Big enough for MBEDTLS_SSL_IN/OUT_BUFFER_LEN with a 16 KB record
// (16,384 + header, IV, MAC and padding); checked against what mbedTLS
// asks for in the log at the first connection.
constexpr size_t kSlot = 17408;
constexpr size_t kLarge = 8192;   // requests this big or more use a slot
constexpr int    kSlots = 2;

alignas(16) uint8_t s_slots[kSlots][kSlot];
bool                s_used[kSlots] = {};
portMUX_TYPE        s_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t            s_served = 0, s_overflowed = 0;
int                 s_logged = 0;

void *arenaCalloc(size_t n, size_t size)
{
    const size_t bytes = n * size;
    if (bytes >= kLarge)
    {
        if (s_logged < 6)
        {
            ++s_logged;
            Log.printf("TLS arena: %u-byte buffer asked for\n", (unsigned)bytes);
        }
        if (bytes <= kSlot)
        {
            int slot = -1;
            portENTER_CRITICAL(&s_mux);
            for (int i = 0; i < kSlots; ++i)
                if (!s_used[i]) { s_used[i] = true; slot = i; break; }
            portEXIT_CRITICAL(&s_mux);
            if (slot >= 0)
            {
                ++s_served;
                memset(s_slots[slot], 0, bytes);
                return s_slots[slot];
            }
        }
        ++s_overflowed;   // both taken, or larger than a slot: the heap, as before
    }
    return esp_mbedtls_mem_calloc(n, size);
}

void arenaFree(void *p)
{
    if (!p) return;
    const uint8_t *b = (const uint8_t *)p;
    if (b >= &s_slots[0][0] && b < &s_slots[0][0] + sizeof(s_slots))
    {
        const int slot = (int)((b - &s_slots[0][0]) / kSlot);
        portENTER_CRITICAL(&s_mux);
        s_used[slot] = false;
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    esp_mbedtls_mem_free(p);
}
}

void install()
{
    mbedtls_platform_set_calloc_free(arenaCalloc, arenaFree);
    Log.printf("TLS arena: %u bytes for connection buffers\n", (unsigned)sizeof(s_slots));
}

bool slotsFree()
{
    portENTER_CRITICAL(&s_mux);
    bool free = true;
    for (bool u : s_used) free = free && !u;
    portEXIT_CRITICAL(&s_mux);
    return free;
}

uint32_t served() { return s_served; }
uint32_t overflowed() { return s_overflowed; }
}
