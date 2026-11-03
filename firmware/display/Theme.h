#pragma once
/*
Purpose: The panel's design tokens, shared by every screen, and the one
title band the list screens open with.

Colour has a job on the panel, not a mood:
  Ink    the thing you read first (call signs, figures)
  Soft   what qualifies it (labels, units, the right of a title)
  Faint  rules, empty states, the third line
  Amber  Glideslope itself and arrivals: the title tick, the headline figure
  Cyan   departures
  Sky    secondary data that wants to be told apart from Ink (types, routes)
  Green  good news: landed, a new sticker
  Red    only for something going wrong (emergency, go-around count)
An airline's own colour stands in for Amber on that flight's screens.
*/
#include "display/FrameCanvas.h"
#include "display/Fonts.h"

namespace Theme
{
    constexpr Rgb Ink   {235, 238, 245};
    constexpr Rgb Soft  {150, 160, 180};
    constexpr Rgb Faint { 80,  88, 106};
    constexpr Rgb Rule  { 34,  40,  54};
    constexpr Rgb Amber {255, 185,  60};
    constexpr Rgb Cyan  { 60, 200, 225};
    constexpr Rgb Sky   {150, 200, 255};
    constexpr Rgb Green {110, 220, 140};
    constexpr Rgb Red   {255,  95,  80};

    constexpr int BandH = 10;   // the title band's height; content starts below it

    // The title band: a dark band fading down, a tick in the screen's
    // colour, the title on the left and a soft extra (clock, count) right.
    inline void titleBand(FrameCanvas &c, const char *title, const char *extra, Rgb tick = Amber, Rgb ink = Ink)
    {
        for (int y = 0; y < BandH; ++y)
        {
            const float k = y / (float)BandH;
            const Rgb col{(uint8_t)(20 - 14 * k), (uint8_t)(26 - 18 * k), (uint8_t)(42 - 28 * k)};
            for (int x = 0; x < FrameCanvas::W; ++x) c.set(x, y, col);
        }
        c.fillRect(0, 1, 2, 8, tick);
        c.print(Fonts::Label, 5, 1, title, ink);
        if (extra && extra[0])
            c.print(Fonts::Label, FrameCanvas::W - 2 - FrameCanvas::width(Fonts::Label, extra), 1, extra, Soft);
    }

    // Which page of a set is up, as dots at the right of the title band.
    inline void pageDots(FrameCanvas &c, int page, int count, Rgb on = Amber)
    {
        for (int i = 0; i < count; ++i)
        {
            const int x = FrameCanvas::W - 4 - (count - 1 - i) * 4;
            c.fillRect(x, 4, 2, 2, i == page ? on : Faint);
        }
    }
}
