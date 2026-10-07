#include "display/BootSplash.h"
#include "display/InfoScreens.h"
#include <math.h>

namespace
{
    const Rgb kText     {235, 240, 245};
    const Rgb kHorizon  { 50, 120, 130};
    const Rgb kStar     {150, 200, 255};
    const Rgb kBars     {200, 200, 205};
    const Rgb kLightDim {140,  92,  25};
    const Rgb kLightTail{255, 200, 110};
    const Rgb kLightHead{255, 255, 255};
    const Rgb kLabel    { 90, 200, 210};

    constexpr uint32_t kDotMs   = 300;   // "SCANNING..." dot cadence
    constexpr uint32_t kChaseMs = 70;    // approach-light step
    constexpr uint32_t kTwinkleMs = 260;

    const int kStars[][2] = { {14, 19}, {40, 17}, {71, 20}, {99, 16}, {121, 21} };
}

void BootSplash::render(FrameCanvas &c, uint32_t tMs, const char *runway)
{
    c.clear();

    // "SCANNING" plus 0-3 dots; position fixed so the word doesn't shuffle.
    const int dots = (int)((tMs / kDotMs) % 4);
    c.text((FrameCanvas::W - FrameCanvas::textWidth("SCANNING...")) / 2, 8, "SCANNING...", kText,
           1, 0, 8 + dots);

    // Dotted horizon with twinkling stars above it.
    for (int x = 0; x < FrameCanvas::W; x += 3) c.set(x, 26, kHorizon);
    for (int i = 0; i < (int)(sizeof(kStars) / sizeof(kStars[0])); ++i)
    {
        const float k = 0.35f + 0.65f * (0.5f + 0.5f * sinf(tMs / (float)kTwinkleMs + i * 1.7f));
        c.set(kStars[i][0], kStars[i][1], FrameCanvas::scale(kStar, k));
    }

    // Runway threshold bars at the left, approach lights chasing towards them.
    for (int x = 2; x <= 10; x += 2)
        for (int y = 36; y <= 40; ++y)
            c.set(x, y, kBars);

    constexpr int kFirst = 118, kLast = 16, kStep = 8;
    constexpr int kLights = (kFirst - kLast) / kStep + 1;
    const int head = (int)((tMs / kChaseMs) % (kLights + 3));   // +3: a pause before the next run
    for (int j = 0; j < kLights; ++j)
    {
        const int x = kFirst - j * kStep;
        const Rgb col = j == head ? kLightHead : j == head - 1 ? kLightTail : kLightDim;
        c.set(x, 38, col);
        c.set(x + 1, 38, col);
    }

    // Runway in use, e.g. "WEST 27L ARR" or "27L UNTIL 15:00".
    char label[20];
    InfoScreens::runwaySummary(runway, label, sizeof(label), true);
    if (!label[0]) snprintf(label, sizeof(label), "LHR");
    c.text((FrameCanvas::W - FrameCanvas::textWidth(label)) / 2, 50, label, kLabel);
}
