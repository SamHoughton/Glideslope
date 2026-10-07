#pragma once
/*
Purpose: Work out what an aircraft is doing relative to the airport's runways —
approach to a specific threshold, inbound, departed or overflight — plus the
distance to go, an ETA, and a 0..1 progress value for the card's strip.
Inputs come from FlightInfo (position, track, altitude, groundspeed,
vertical rate, origin/destination).
*/
#include <Arduino.h>
#include "models/FlightInfo.h"

struct ApproachStatus
{
    enum Phase : uint8_t { Unknown, Approach, Landing, Inbound, Departed, Outbound, Overflight };

    Phase  phase       = Unknown;
    char   runway[4]   = "";     // e.g. "27L"; empty when no runway applies
    float  distKm      = NAN;    // to the runway threshold (approach) or the airport (others)
    float  etaSec      = NAN;    // only for Approach / Landing
    float  progress    = NAN;    // 0 = far out, 1 = at the threshold
};

namespace ApproachModel
{
    ApproachStatus evaluate(const FlightInfo &f);

    // Dead-reckon an Approach/Landing status forward by ageMs at the given
    // groundspeed, so the ETA counts down and the marker moves between fetches.
    ApproachStatus advance(const ApproachStatus &s, double gsKt, uint32_t ageMs);

    // Status label for the card, e.g. "APPROACH 27L", "DEPARTED 09R", "OVERFLIGHT".
    void label(const ApproachStatus &s, char *buf, size_t len);
}
