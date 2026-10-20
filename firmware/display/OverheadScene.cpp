#include "display/OverheadScene.h"
#include "display/Fonts.h"
#include <math.h>

namespace
{
// Text with a dark shadow, so it reads over a bright sky.
void shadowText(FrameCanvas &c, int x, int y, const char *s, Rgb col, const Fonts::Font &f = Fonts::Label)
{
    c.print(f, x + 1, y + 1, s, Rgb{0, 0, 0});
    c.print(f, x, y, s, col);
}

float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
}

void OverheadScene::render(FrameCanvas &c, const Info &info, uint32_t tMs, const Sky::Look &sky)
{
    c.clear();
    Sky::drawSky(c, sky, tMs, FrameCanvas::H);   // all sky: we are looking up

    // Size by height: below 8,000 ft the big scene sprite, below 20,000 the
    // card sprite, above that half size.
    const float alt = isnan(info.altFt) ? 10000 : info.altFt;
    const AircraftSprites::Sprite &big = AircraftSprites::getScene(info.kind);
    const AircraftSprites::Sprite &mid = AircraftSprites::get(info.kind);
    const bool half = alt >= 20000;
    const AircraftSprites::Sprite &s = alt < 8000 ? big : mid;
    const int w = half ? (s.w + 1) / 2 : s.w, h = half ? (s.h + 1) / 2 : s.h;

    // Across the whole panel over the scene, a little faster when low.
    const float u = clamp01((float)tMs / DURATION_MS);
    const float x0 = -w - 2.0f, x1 = FrameCanvas::W + 2.0f;
    const float fx = info.rightward ? x0 + u * (x1 - x0) : x1 - u * (x1 - x0) - w;
    const int px = (int)lroundf(fx);
    const int py = (int)lroundf(30 - h / 2.0f + (half ? -6 : 0));

    // Contrails above 25,000 ft: two lines from behind the aircraft, fading
    // and spreading with distance.
    if (alt >= 25000)
    {
        const int tailX = info.rightward ? px : px + w - 1;
        const int dir = info.rightward ? -1 : 1;
        const int y = py + h / 2;
        for (int d = 2; d < 110; ++d)
        {
            const int x = tailX + dir * d;
            if (x < 0 || x >= FrameCanvas::W) break;
            const float k = 0.85f * (1 - d / 110.0f);
            const int spread = d / 28;
            for (int line : {-1, 1})
            {
                const int yy = y + line * (1 + spread);
                const Rgb under = FrameCanvas::unpack(c.get(x, yy));
                c.set(x, yy, Rgb{(uint8_t)(under.r + (235 - under.r) * k), (uint8_t)(under.g + (240 - under.g) * k),
                                 (uint8_t)(under.b + (245 - under.b) * k)});
            }
        }
    }
    if (half) AircraftSprites::drawHalf(c, s, px, py, info.livery, info.rightward, !info.known);
    else      AircraftSprites::draw(c, s, px, py, info.livery, info.rightward, !info.known);

    // Label chip top left, altitude top right.
    char label[24];
    strlcpy(label, info.label, sizeof(label));
    Fonts::sentenceCase(label);
    const int lw = FrameCanvas::width(Fonts::Label, label);
    for (int y = 0; y < 9; ++y)
        for (int x = 0; x < lw + 4; ++x) c.set(x + 1, y + 1, FrameCanvas::scale(info.labelColour, 0.35f));
    c.print(Fonts::Label, 3, 2, label, info.labelColour);
    if (!isnan(info.altFt))
    {
        char a[16], digits[12];
        snprintf(digits, sizeof(digits), "%d", (int)lroundf(info.altFt / 50.0f) * 50);
        // Thousands separator.
        const int n = strlen(digits);
        int o = 0;
        for (int i = 0; i < n; ++i)
        {
            if (i && (n - i) % 3 == 0) a[o++] = ',';
            a[o++] = digits[i];
        }
        strcpy(a + o, " ft");
        shadowText(c, FrameCanvas::W - 2 - FrameCanvas::width(Fonts::Label, a), 2, a, Rgb{235, 240, 245});
    }

    // Bottom: the flight in its colour (double size when it fits), then where
    // it's going, the type on the right.
    const bool big2 = FrameCanvas::width(Fonts::Large, info.ident) <= 70;
    if (big2) shadowText(c, 2, 40, info.ident, info.accent, Fonts::Large);
    else      shadowText(c, 2, 44, info.ident, info.accent, Fonts::Medium);
    if (info.where[0]) shadowText(c, 2, 55, info.where, Rgb{235, 240, 245});
    if (info.type[0])
        shadowText(c, FrameCanvas::W - 2 - FrameCanvas::width(Fonts::Label, info.type), 55, info.type, Rgb{150, 200, 235});
}
