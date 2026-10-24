#pragma once
/*
Purpose: What the display's source files share: the frames, the timing
constants, the state of the requests the web task leaves for the display
task, and a few helpers. Only adapters/NeoMatrixDisplay*.cpp and
adapters/Display*.cpp include it.
*/
#include "adapters/NeoMatrixDisplay.h"
#include "interfaces/BaseLogoStore.h"
#include "config/HardwareConfiguration.h"
#include "config/RuntimeConfig.h"
#include "display/ApproachModel.h"
#include "display/CardRenderer.h"
#include "display/AircraftSprites.h"
#include "display/FlyAcross.h"
#include "display/BootSplash.h"
#include "display/LandingScene.h"
#include "display/MapRenderer.h"
#include "display/RareSpotter.h"
#include "display/InfoScreens.h"
#include "config/Airport.h"
#include "config/Airports.h"
#include "display/DemoLogo.h"
#include "utils/TelnetLogger.h"
#include "utils/Notify.h"
#include <esp_task_wdt.h>
#include "utils/StageTrace.h"

// Frames (16 KB and 12 KB): the one being drawn, and a snapshot of what a
// transition starts from. The panel mirror, g_shownFrame, is in NeoMatrixDisplay.h.
inline FrameCanvas g_workFrame;
inline PackedFrame g_oldFrame;   // 12 bits a pixel: 4 KB less than a FrameCanvas

static constexpr size_t        kMaxQueue        = 5;
static constexpr unsigned long kSeenCooldownMs  = 15UL * 60 * 1000;  // don't re-announce within this
static constexpr unsigned long kStaleEntryMs    = 3UL * 60 * 1000;   // drop queued flights not refreshed
static constexpr unsigned long kMessageMs       = 4000;
static constexpr unsigned long kIdleAfterMs     = 5UL * 60 * 1000;   // card -> scanning screen
static constexpr unsigned long kLandedHoldMs    = 8000;              // LANDED card, then a break
static constexpr unsigned long kShortCardMs     = 8000;              // departure / overflight card
static constexpr float         kImminentSec     = 45.0f;             // lands this soon: skip the break
static constexpr unsigned long kMapDwellMs      = 20000;             // rotation: map ...
static constexpr unsigned long kScreenDwellMs   = 10000;             // ... each other screen
static constexpr unsigned long kStatsPageMs     = 7000;              // each extra stats page
static constexpr float         kAltEaseSec      = 0.8f;              // altitude smoothing time constant
static constexpr uint32_t      kAmbientFadeMs   = 700;               // card -> map cross-fade
static constexpr uint32_t      kFlourishMs      = 1600;              // rare-spot banner before the fly-across
static constexpr unsigned long kCaptionMs       = 1200;              // mode-change caption
static constexpr int           kButtonPin       = 17;                // HD-WF2 test key (0 = pressed)
static constexpr unsigned long kFinalMemoryMs   = 180000;            // "was on final" lasts this long
static constexpr float         kGoAroundClimbFpm = 500.0f;           // climbing faster than this ...
static constexpr float         kGoAroundMaxFt   = 5000.0f;           // ... below this, after final
static constexpr unsigned long kLandedSilentMs  = 15000;             // gone from the data this long at the threshold = landed
static constexpr float         kLandedReportFt  = 200.0f;            // or reported this low near the threshold
static constexpr unsigned long kAlertMs         = 12000;             // emergency-squawk alert on screen
static constexpr unsigned long kRunwayChangeMs  = 7000;              // RUNWAY CHANGE announcement
static constexpr float         kFreshDepartureFt = 4000.0f;          // take-off scene below this
static constexpr uint32_t      kAnimFrameMs     = 20;                // 50 fps during the fly-across
static constexpr uint32_t      kCardFrameMs     = 50;                // 20 fps otherwise

// Requests from the web task (set there, read by the display task), and state.
inline volatile uint32_t s_framesDrawn = 0;
inline volatile uint32_t s_galleryRequestMs = 0;   // set by the web task
inline unsigned long     s_galleryUntil     = 0;
inline volatile uint32_t s_splashRequestMs  = 0;   // set by the web task
inline unsigned long     s_splashUntil      = 0;
inline volatile uint32_t s_mapRequestMs  = 0;   // set by the web task
inline unsigned long     s_mapPreviewUntil = 0;
inline volatile unsigned long s_demoUntil = 0;   // night watch stands aside until then (a web demo)
inline volatile uint8_t s_screenPreview = 0;
inline unsigned long    s_screenPreviewUntil = 0;
inline volatile uint8_t s_replayRequest = 0;
inline volatile bool s_landingReplay = false;
inline volatile bool s_rareDemo = false;
inline volatile uint8_t s_moodDemo = 0;   // 8.2: a mood banner (1 fog, 2 storm, 3 runway due)
inline volatile bool s_goAroundDemo = false;
inline volatile bool s_takeoffDemo = false;
inline volatile bool s_alertDemo = false;
inline volatile bool s_spotDemo = false;
inline volatile bool s_runwayDemo = false;
inline char          s_skyPreview[24] = "";
inline volatile unsigned long s_skyPreviewUntil = 0;
inline volatile bool s_showcaseRequest = false;
inline volatile bool s_skyDemo = false;
inline volatile uint32_t s_frameShowMs = UINT32_MAX;
inline char          s_panelMsg[20] = "";
inline volatile bool s_panelMsgPending = false;
inline volatile uint32_t s_panelMsgMs = 4000;

namespace
{
    struct Lock
    {
        SemaphoreHandle_t m;
        explicit Lock(SemaphoreHandle_t mm) : m(mm) { if (m) xSemaphoreTake(m, portMAX_DELAY); }
        ~Lock() { if (m) xSemaphoreGive(m); }
    };

    inline uint8_t effectiveBrightness()
    {
        // Screen off at night stays a clean switch; otherwise ease between the two.
        if (g_config.night_brightness == 0 || !g_config.night_follow_sun)
            return isNightActive() ? g_config.night_brightness : g_config.display_brightness;
        const float k = nightLevel();
        return (uint8_t)lroundf(g_config.display_brightness + (g_config.night_brightness - (int)g_config.display_brightness) * k);
    }

    // Ordering for "most worth showing next": aircraft on approach first, then nearest.
    inline float priority(const FlightInfo &f)
    {
        const ApproachStatus s = ApproachModel::evaluate(f);
        const float d = isnan(f.distance_km) ? 999.0f : (float)f.distance_km;
        const bool onApproach = s.phase == ApproachStatus::Approach || s.phase == ApproachStatus::Landing;
        return (onApproach ? 0.0f : 1000.0f) + d;
    }
}
