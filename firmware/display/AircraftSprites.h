#pragma once
/*
Purpose: Side-profile aircraft sprites (nose left) for the 128x64 panel,
picked by ICAO aircraft type code, painted in the airline's colours.

Twenty types, each at two sizes: the card / fly-across size (up to 44x17)
and a larger one with the gear down for the landing and take-off scenes.
They are generated from each type's proportions by tools/sprite_gen.py
(display/SpriteData.h), shaded from a highlight along the top to a shadow
under the belly. Characters, one per pixel:
  '.' transparent  '^' body highlight  '#' body  '-' cheatline  '+' belly
  '_' belly shadow 'a' 'b' soft edges (body, belly)  'w' windows  'c' cockpit
  'r' fin  't' fin shade  'R' fin accent  'g' 'G' wing / tailplane light, dark
  'e' 'E' engine cowl lit, shaded  'i' intake / exhaust  'd' gear  'p' prop
Liveries (about 45 airlines, colours only) come from the same file; any
other airline flies white with its logo colour on the fin. Unknown types
use the A320 drawn in greys.
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"
#include "models/FlightInfo.h"

namespace AircraftSprites
{
    enum Kind : uint8_t
    {
        A320, A321, B737, A220, EJet, CRJ, B757, B767, B787, A330, A350, B777,
        A340, B747, A380, ATR, Q400, Bizjet, Helicopter, LightAircraft,
        KindCount
    };

    struct Sprite
    {
        const char        *name;
        uint8_t            w, h;
        const char *const *rows;
    };

    // The colours an aircraft is painted in. From a single colour: white with
    // that colour on the fin (what every airline got before liveries).
    struct Livery
    {
        Rgb body{215, 220, 228}, belly{150, 156, 168}, stripe{215, 220, 228};
        Rgb tail{90, 170, 255}, accent{50, 100, 160}, engine{175, 180, 190};
        Livery() = default;
        Livery(Rgb fin) : tail(fin), accent(FrameCanvas::scale(fin, 0.6f)) {}
    };

    const Sprite &get(Kind k);        // card and fly-across
    const Sprite &getScene(Kind k);   // landing / take-off scenes: larger, gear down

    // Classify an ICAO type designator (e.g. "A20N", "B77W", "EC35").
    // known is set false when the code is unrecognised (fallback sprite).
    Kind classify(const String &icaoType, bool &known);

    // The airline's livery (from the call sign, logo or operator code), or
    // white with accent on the fin.
    Livery liveryFor(const FlightInfo &f, Rgb accent);

    // Draw with the top-left at (x, y). flipX mirrors the sprite (nose right).
    // grey draws the whole sprite in greys (the unknown-type fallback).
    // outline clears a 1px black halo first so it reads over busy backgrounds.
    void draw(FrameCanvas &c, const Sprite &s, int x, int y, const Livery &livery,
              bool flipX = false, bool grey = false, bool outline = false);
}
