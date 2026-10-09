#include "display/FlyAcross.h"
#include <math.h>

namespace
{
    constexpr int   kFlashWidth = 10;     // px of white flash trailing the plane
    constexpr float kFlashPeak  = 0.7f;   // blend towards white at the plane's tail

    // Vertical path: centred on kMidY, sloped by the vertical rate.
    constexpr int   kMidY        = 18;    // sprite top at mid-crossing
    constexpr int   kMaxRisePx   = 28;    // steepest climb/descent across the panel
    constexpr float kPxPer1000Fpm = 28.0f;
    constexpr float kLevelFpm    = 150.0f; // below this the crossing is flat

    float facingDegrees(const char *name)
    {
        static const struct { const char *n; float d; } kPoints[] = {
            {"N", 0}, {"NNE", 22.5f}, {"NE", 45}, {"ENE", 67.5f},
            {"E", 90}, {"ESE", 112.5f}, {"SE", 135}, {"SSE", 157.5f},
            {"S", 180}, {"SSW", 202.5f}, {"SW", 225}, {"WSW", 247.5f},
            {"W", 270}, {"WNW", 292.5f}, {"NW", 315}, {"NNW", 337.5f},
        };
        if (name)
            for (const auto &p : kPoints)
                if (strcmp(name, p.n) == 0) return p.d;
        return 0;
    }
}

FlyAcross::Path FlyAcross::pathFor(double headingDeg, double verticalRateFpm, const char *screenFacing)
{
    Path p;
    // Looking towards `facing`, the viewer's right is facing + 90°. The plane
    // moves rightward on screen when its track has a component that way.
    if (!isnan(headingDeg))
    {
        const float rel = (float)(headingDeg - facingDegrees(screenFacing)) * (float)M_PI / 180.0f;
        p.rightward = sinf(rel) > 0;
    }

    float rise = 0;   // positive = climbs on screen
    if (!isnan(verticalRateFpm) && fabs(verticalRateFpm) >= kLevelFpm)
        rise = (float)verticalRateFpm / 1000.0f * kPxPer1000Fpm;
    if (rise >  kMaxRisePx) rise =  kMaxRisePx;
    if (rise < -kMaxRisePx) rise = -kMaxRisePx;
    p.startY = (int)lroundf(kMidY + rise / 2);
    p.endY   = (int)lroundf(kMidY - rise / 2);
    return p;
}

void FlyAcross::compose(FrameCanvas &out, const FrameCanvas &oldFrame, uint32_t tMs,
                        const AircraftSprites::Sprite &sprite, const AircraftSprites::Livery &livery, bool greySprite,
                        const Path &path)
{
    const float q = tMs >= DURATION_MS ? 1.0f : (float)tMs / DURATION_MS;

    // Enter just off one edge, leave fully off the other.
    const int offRight = FrameCanvas::W + 2;
    const int offLeft  = -(int)sprite.w - 2;
    const int startX   = path.rightward ? offLeft : offRight;
    const int endX     = path.rightward ? offRight : offLeft;
    const int planeX   = (int)lroundf(startX + q * (endX - startX));
    const int planeY   = (int)lroundf(path.startY + q * (path.endY - path.startY));
    const int edge     = planeX + sprite.w / 2;   // boundary between old and new

    uint16_t       *px  = out.pixels();
    const uint16_t *old = oldFrame.pixels();
    for (int y = 0; y < FrameCanvas::H; ++y)
        for (int x = 0; x < FrameCanvas::W; ++x)
        {
            const int i = y * FrameCanvas::W + x;
            // Distance behind the plane (into the newly revealed card).
            const int d = path.rightward ? edge - x : x - edge;
            if (d < 0) { px[i] = old[i]; continue; }
            if (d >= kFlashWidth || px[i] == 0) continue;
            const float m = (1.0f - (float)d / kFlashWidth) * kFlashPeak;
            Rgb c = FrameCanvas::unpack(px[i]);
            c = { (uint8_t)(c.r + (255 - c.r) * m), (uint8_t)(c.g + (255 - c.g) * m),
                  (uint8_t)(c.b + (255 - c.b) * m) };
            px[i] = FrameCanvas::pack(c);
        }

    // Sprites are drawn nose-left; mirror them when flying rightward.
    AircraftSprites::draw(out, sprite, planeX, planeY, livery, path.rightward, greySprite, true);
}
