#pragma once
/*
Purpose: Which runways are in use, from the traffic itself: every aircraft in
range, every fetch. One lined up on final for a runway (ApproachModel) counts
towards that runway's arrivals; one climbing out along a runway
(ApproachModel::climbingOut) towards its departures. Scores fade over a few
minutes, so the answer follows the airport without flickering on one odd
aircraft, and several runways can be in use at once (parallel arrivals at
Atlanta or Schiphol; Heathrow's separate arrival and departure runways).

A runway is "in use" when it has seen traffic in the last 10 minutes; with
no arrivals for 30 minutes there is no arrival runway at all (overnight).

A change of landing direction (the main arrival runway now points more than
90 degrees away from the last one, seen by at least two aircraft) is
reported once, for the panel to announce. Swapping between parallel runways
(Heathrow's 15:00 alternation) is only logged.
*/
#include <Arduino.h>
#include <vector>
#include "models/StateVector.h"

class RunwayTracker
{
public:
    // Every aircraft in range, once per fetch. Returns true when the landing
    // direction has just changed (see changeFrom / changeTo).
    bool update(const std::vector<StateVector> &states, unsigned long nowMs);

    // Runways in use, busiest first, space separated ("27L", "26R 26L"); "" if none.
    void arrivals(char *out, size_t len) const;
    void departures(char *out, size_t len) const;
    // The busiest arrival runway ("" if none): the one the screens show.
    const char *mainArrival() const { return _main; }

    // The last change of landing direction.
    const char *changeFrom() const { return _from; }
    const char *changeTo() const { return _main; }

private:
    static constexpr int kMax = 16;   // runway ends (Airport::kMaxRunways)
    struct Slot
    {
        float         arr = 0, dep = 0;          // fading scores
        unsigned long arrMs = 0, depMs = 0;      // last aircraft seen
        uint32_t      arrIds[4] = {}, depIds[4] = {};   // a few recent aircraft (distinct count)
    };
    Slot          _slots[kMax];
    unsigned long _lastMs = 0;
    char          _main[4] = "", _from[4] = "";
    float         _mainCourse = NAN;              // course of the last announced direction

    void list(bool arrivalsWanted, char *out, size_t len) const;
    static int distinct(const uint32_t ids[4]);
};
