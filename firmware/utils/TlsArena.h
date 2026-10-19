#pragma once
/*
Purpose: Room for TLS that the rest of the heap can't break up. mbedTLS
gives every connection two ~16.7 KB record buffers; on this board's heap,
after hours of small allocations, two blocks that size are often not
there, so once the feed's connection was closed (for the weather, say) it
could not reopen and the board went without positions until it restarted.

Two fixed slots, reserved once at start, take mbedTLS's large allocations
(installed with mbedtls_platform_set_calloc_free); everything smaller goes
to the heap as before. One connection's buffers fit at a time, which is
how the board uses TLS (the feed's connection is closed before any other).
The memory is the same as before: those buffers just live here instead.
*/
#include <Arduino.h>

namespace TlsArena
{
// Route mbedTLS's allocations through the arena (call before any TLS).
void install();
// Whether both slots are free (no connection holds them).
bool slotsFree();
// Large allocations served from the slots, and refused for want of one.
uint32_t served();
uint32_t overflowed();
}
