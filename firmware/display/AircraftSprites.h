#pragma once
/*
Purpose: Side-profile aircraft sprites (nose left) for the 128x64 panel,
picked by ICAO aircraft type code.

One sprite per type (up to 44x16), used on the card and for the fly-across;
edit them in tools/sprite_workbench.py. Sprites are C arrays of row strings, one character per pixel:
  '.' transparent   '#' fuselage    '+' belly shade   'w' windows
  'c' cockpit glass 'r' tail (airline accent)        'R' tail shade
  'g' wing / tailplane   'd' engine / gear   'e' engine cowl   'p' prop / rotor
Unknown types fall back to the narrowbody drawn entirely in grey.
*/
#include <Arduino.h>
#include "display/FrameCanvas.h"

namespace AircraftSprites
{
    enum Kind : uint8_t
    {
        Narrowbody, WidebodyTwin, A380, B747, RegionalJet,
        Turboprop, Bizjet, Helicopter, LightAircraft,
        KindCount
    };

    struct Sprite
    {
        const char        *name;
        uint8_t            w, h;
        const char *const *rows;
    };

    const Sprite &get(Kind k);

    // Classify an ICAO type designator (e.g. "A20N", "B77W", "EC35").
    // known is set false when the code is unrecognised (fallback sprite).
    Kind classify(const String &icaoType, bool &known);

    // Draw with the top-left at (x, y). flipX mirrors the sprite (nose right).
    // grey draws the whole sprite in greys (the unknown-type fallback).
    // outline clears a 1px black halo first so it reads over busy backgrounds.
    void draw(FrameCanvas &c, const Sprite &s, int x, int y, Rgb tail,
              bool flipX = false, bool grey = false, bool outline = false);
}
