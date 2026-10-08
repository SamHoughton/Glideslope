#pragma once
/*
Purpose: Aircraft in holding patterns, and how long they have been there.

Two ways in:
- Named stacks (Heathrow's BNN, LAM, BIG and OCK; a pack can list its own):
  the main loop samples the airspace around each fix every 3 minutes, in
  place of a position fetch (noteStack). An aircraft within 9 km of the fix,
  between 5,000 and 20,000 ft, on two samples at least 2.5 minutes apart is
  holding there: anything passing through has flown well clear by then.
- Anywhere in range: an aircraft that keeps turning the same way (more than
  300 degrees with the turning fading over a few minutes, so a long
  vectoring turn onto final does not count) is holding (noteStates). Any
  airport, no setup.

An aircraft leaves the count once it stops turning, leaves the fix or has
not been seen for 2.5 minutes (a stack's aircraft: missing from its next sample).
*/
#include <Arduino.h>
#include <vector>
#include "models/StateVector.h"

class HoldTracker
{
public:
    // Every aircraft in range, once per fetch.
    void noteStates(const std::vector<StateVector> &states, unsigned long nowMs);
    // The aircraft around named stack i (Airport::holds[i]), once a minute.
    void noteStack(int stack, const std::vector<StateVector> &states, unsigned long nowMs);

    struct Row
    {
        char    name[8];      // "BNN", or "OTHER" for holds away from a named stack
        uint8_t count;
        uint8_t longestMin;   // minutes the longest-holding aircraft has been there
    };
    // Named stacks first (all of them, even empty), then OTHER if any; returns rows filled.
    int rows(Row *out, int max, unsigned long nowMs) const;
    int total(unsigned long nowMs) const;

private:
    static constexpr int kMax = 40;
    struct Entry
    {
        uint32_t      id = 0;
        float         heading = NAN;
        float         turn = 0;           // degrees, signed, fading
        unsigned long seenMs = 0;         // last report of any kind
        unsigned long headingMs = 0;      // when heading was taken
        int8_t        stack = -1;         // named stack it is near, -1 none
        uint8_t       stackSamples = 0;
        unsigned long stackFirstMs = 0, stackLastMs = 0;
        bool          turning = false;    // holding by the turning rule
        unsigned long holdSinceMs = 0;
    };
    Entry _e[kMax];

    Entry *find(uint32_t id, unsigned long now);
    bool   holding(const Entry &e, unsigned long now) const;
};
