/*
Purpose: Drive the HUB75 panel via ESP32-HUB75-MatrixPanel-I2S-DMA.
Responsibilities:
- Initialise the matrix (HD-WF2 pin map when built with HD_WF2).
- Queue new contacts and fly each one in over the previous card.
- Render frames into an off-screen FrameCanvas on a dedicated task and push
  only the pixels that changed to the panel.
*/
#include "adapters/NeoMatrixDisplay.h"
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
#include "utils/TelnetLogger.h"
#include <esp_task_wdt.h>
#include "utils/StageTrace.h"

// Static frames (16 KB each): the one being drawn, a mirror of the panel, and
// a snapshot of the outgoing card during a fly-across.
static FrameCanvas g_workFrame;
static FrameCanvas g_oldFrame;
FrameCanvas        g_shownFrame;

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
static constexpr float         kFreshDepartureFt = 4000.0f;          // take-off scene below this
static constexpr uint32_t      kAnimFrameMs     = 20;                // 50 fps during the fly-across
static constexpr uint32_t      kCardFrameMs     = 50;                // 20 fps otherwise

static volatile uint32_t s_framesDrawn = 0;
uint32_t displayFramesDrawn() { return s_framesDrawn; }

static volatile uint32_t s_galleryRequestMs = 0;   // set by the web task
static unsigned long     s_galleryUntil     = 0;
static volatile uint32_t s_splashRequestMs  = 0;   // set by the web task
static unsigned long     s_splashUntil      = 0;

void requestSplashPreview(uint32_t durationMs) { s_splashRequestMs = durationMs ? durationMs : 1; }

static volatile uint32_t s_mapRequestMs  = 0;   // set by the web task
static unsigned long     s_mapPreviewUntil = 0;

void requestMapPreview(uint32_t durationMs) { s_mapRequestMs = durationMs ? durationMs : 1; }

// Ambient screen previews from the web page: 1 stats, 2 clock.
static volatile uint8_t s_screenPreview = 0;
static unsigned long    s_screenPreviewUntil = 0;

void requestScreenPreview(uint8_t which, uint32_t durationMs)
{
    s_screenPreviewUntil = millis() + durationMs;
    s_screenPreview = which;
}

void requestSpriteGallery(uint32_t durationMs) { s_galleryRequestMs = durationMs ? durationMs : 1; }

// Replay request from the web task: 0 none, 1 real path, 2 force L→R, 3 force R→L.
static volatile uint8_t s_replayRequest = 0;

static volatile bool s_landingReplay = false;
static volatile bool s_rareDemo = false;
static volatile bool s_goAroundDemo = false;
static volatile bool s_takeoffDemo = false;
static volatile bool s_alertDemo = false;

void requestTakeoffDemo() { s_takeoffDemo = true; }

void requestAlertDemo() { s_alertDemo = true; }

static char          s_panelMsg[20] = "";
static volatile bool s_panelMsgPending = false;

void requestPanelMessage(const char *text)
{
    strlcpy(s_panelMsg, text, sizeof(s_panelMsg));
    s_panelMsgPending = true;
}

void requestGoAroundDemo() { s_goAroundDemo = true; }

void requestRareSpotDemo() { s_rareDemo = true; }

void requestLandingReplay() { s_landingReplay = true; }

void requestFlyAcrossReplay(int forceDirection)
{
    s_replayRequest = forceDirection > 0 ? 2 : forceDirection < 0 ? 3 : 1;
}

namespace
{
    struct Lock
    {
        SemaphoreHandle_t m;
        explicit Lock(SemaphoreHandle_t mm) : m(mm) { if (m) xSemaphoreTake(m, portMAX_DELAY); }
        ~Lock() { if (m) xSemaphoreGive(m); }
    };

    uint8_t effectiveBrightness()
    {
        return isNightActive() ? g_config.night_brightness : g_config.display_brightness;
    }

    // Ordering for "most worth showing next": aircraft on approach first, then nearest.
    float priority(const FlightInfo &f)
    {
        const ApproachStatus s = ApproachModel::evaluate(f);
        const float d = isnan(f.distance_km) ? 999.0f : (float)f.distance_km;
        const bool onApproach = s.phase == ApproachStatus::Approach || s.phase == ApproachStatus::Landing;
        return (onApproach ? 0.0f : 1000.0f) + d;
    }
}

NeoMatrixDisplay::NeoMatrixDisplay() {}

NeoMatrixDisplay::~NeoMatrixDisplay()
{
    if (_task) vTaskDelete(_task);
    if (_matrix)
    {
        delete _matrix;
        _matrix = nullptr;
    }
}

bool NeoMatrixDisplay::initialize()
{
    _lock = xSemaphoreCreateMutex();

    HUB75_I2S_CFG mxconfig(
        HardwareConfiguration::DISPLAY_MATRIX_WIDTH,
        HardwareConfiguration::DISPLAY_MATRIX_HEIGHT,
        HardwareConfiguration::DISPLAY_TILES_X
    );
#ifdef HD_WF2
    // Huidu HD-WF2 (ESP32-S3), HUB75 port X1 — pins from the arduino-esp32 huidu_hd_wf2 variant
    mxconfig.gpio = {
        /*r1*/ 2, /*g1*/ 6, /*b1*/ 10, /*r2*/ 3, /*g2*/ 7, /*b2*/ 11,
        /*a*/ 39, /*b*/ 38, /*c*/ 37, /*d*/ 36, /*e*/ 21,
        /*lat*/ 33, /*oe*/ 35, /*clk*/ 34
    };
#else
    mxconfig.gpio.e    = 18;
#endif
    mxconfig.driver    = HUB75_I2S_CFG::ICN2038S;
    mxconfig.clkphase  = false;
    mxconfig.latch_blanking = 1;
    // Colour depth stays at the default 8 bits: 7 saves 8 KB of DMA buffer but
    // changes the refresh timing, and this panel then flickers green.

    pinMode(kButtonPin, INPUT_PULLUP);

    _matrix = new MatrixPanel_I2S_DMA(mxconfig);
    _matrix->begin();
    _matrix->setRotation(g_config.display_flip ? 2 : 0);
    _lastFlip = g_config.display_flip;
    _matrix->setBrightness8(g_config.display_brightness);
    _lastBrightness = g_config.display_brightness;
    clear();
    return true;
}

void NeoMatrixDisplay::clear()
{
    if (!_matrix) return;
    _matrix->fillScreen(0);
    g_shownFrame.clear();
    _forceFull = false;
}

void NeoMatrixDisplay::startTask()
{
    if (_task || !_matrix) return;
    // Core 0: keeps animation running while the main loop blocks on HTTP fetches.
    xTaskCreatePinnedToCore(taskEntry, "display", 6144, this, 2, &_task, PRO_CPU_NUM);
}

void NeoMatrixDisplay::taskEntry(void *self)
{
    static_cast<NeoMatrixDisplay *>(self)->taskLoop();
}

void NeoMatrixDisplay::taskLoop()
{
    // Watched: if a frame ever hangs, the task watchdog reboots the board
    // rather than leaving the panel frozen.
    esp_task_wdt_add(nullptr);
    for (;;)
    {
        uint32_t waitMs;
        StageTrace::mark(StageTrace::Display, StageTrace::DispWaitLock);
        {
            Lock l(_lock);
            StageTrace::mark(StageTrace::Display, StageTrace::DispRender);
            waitMs = renderFrame(millis());
        }
        StageTrace::mark(StageTrace::Display, StageTrace::DispSleep);
        ++_frames;
        s_framesDrawn = _frames;
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(waitMs ? waitMs : 1));
    }
}

// ── Fetch results in ─────────────────────────────────────────────────────────

void NeoMatrixDisplay::displayFlights(const std::vector<FlightInfo> &flights)
{
    Lock l(_lock);
    const unsigned long now = millis();

    // The aircraft closest to touchdown that isn't on the card: what the
    // landed card moves on to, even if it has been shown before.
    _hasNextApproach = false;
    float bestDist = 1e9f;

    for (const FlightInfo &f : flights)
    {
        if (f.ident.length() == 0) continue;

        _stats.note(f);

        // Remember the runway in use for the scanning screen.
        const ApproachStatus st = ApproachModel::evaluate(f);
        if (st.phase == ApproachStatus::Approach || st.phase == ApproachStatus::Landing)
            strlcpy(_runwayInUse, st.runway, sizeof(_runwayInUse));
        if (st.phase == ApproachStatus::Approach && !isnan(st.distKm) && st.distKm < bestDist &&
            !(_hasCurrent && f.ident == _current.flight.ident))
        {
            bestDist = st.distKm;
            _nextApproach.flight = f;
            _nextApproach.dataMs = now;
            _hasNextApproach = true;
        }

        if (_hasCurrent && f.ident == _current.flight.ident)
        {
            const double prevAlt = _current.flight.baro_altitude;
            _current.flight = f;
            _current.dataMs = now;
            noteApproachProgress(_current, st, prevAlt, now);
        }
        else
        {
            auto q = std::find_if(_queue.begin(), _queue.end(),
                                  [&](const Entry &e) { return e.flight.ident == f.ident; });
            if (q != _queue.end())
            {
                q->flight = f;
                q->dataMs = now;
            }
            else
            {
                auto seen = _seenMs.find(f.ident);
                const bool recent = seen != _seenMs.end() && now - seen->second < kSeenCooldownMs;
                if (!recent)
                {
                    Entry e;
                    e.flight = f;
                    e.dataMs = now;
                    e.accent = CardRenderer::accentFor(f);
                    _queue.push_back(e);
                    Log.printf("Display: new contact %s queued (%u waiting)\n",
                               f.ident.c_str(), (unsigned)_queue.size());
                }
            }
        }
        _seenMs[f.ident] = now;
    }

    // Queued flights that have left the list (or gone stale) are no longer news.
    for (auto it = _queue.begin(); it != _queue.end(); )
    {
        const bool inList = std::any_of(flights.begin(), flights.end(),
                                        [&](const FlightInfo &f) { return f.ident == it->flight.ident; });
        const bool stale  = now - it->dataMs > kStaleEntryMs;
        it = ((!inList && !flights.empty()) || stale) ? _queue.erase(it) : std::next(it);
    }

    // Best candidates first; cap the queue.
    std::stable_sort(_queue.begin(), _queue.end(),
                     [](const Entry &a, const Entry &b) { return priority(a.flight) < priority(b.flight); });
    while (_queue.size() > kMaxQueue)
        _queue.pop_back();

    for (auto it = _seenMs.begin(); it != _seenMs.end(); )
        it = (now - it->second > 2 * kSeenCooldownMs) ? _seenMs.erase(it) : std::next(it);

    if (_hasNextApproach)
        _nextApproach.accent = CardRenderer::accentFor(_nextApproach.flight);
}

void NeoMatrixDisplay::setArrivals(const InfoScreens::Arrival *rows, int n)
{
    Lock l(_lock);
    _arrivalCount = min(n, InfoScreens::kMaxArrivals);
    for (int i = 0; i < _arrivalCount; ++i) _arrivals[i] = rows[i];
}

void NeoMatrixDisplay::setWeather(const Metar &m)
{
    char line[24];
    Weather::line(m, line, sizeof(line));
    Lock l(_lock);
    _metar = m;
    strlcpy(_weather, line, sizeof(_weather));
}

void NeoMatrixDisplay::raiseAlert(const char *code, const char *meaning, const char *ident, const char *detail)
{
    Lock l(_lock);
    strlcpy(_alertCode, code, sizeof(_alertCode));
    strlcpy(_alertMeaning, meaning, sizeof(_alertMeaning));
    strlcpy(_alertIdent, ident, sizeof(_alertIdent));
    strlcpy(_alertDetail, detail, sizeof(_alertDetail));
    _alertStartMs = millis();
    _alertActive  = true;
}

bool NeoMatrixDisplay::isFreshDeparture(const FlightInfo &f) const
{
    if (isnan(f.baro_altitude) || f.baro_altitude > kFreshDepartureFt) return false;
    return ApproachModel::evaluate(f).phase == ApproachStatus::Departed;
}

bool NeoMatrixDisplay::currentFlight(FlightInfo &out)
{
    Lock l(_lock);
    if (!_hasCurrent) return false;
    // Copy without the 2 KB logo: this runs every second for the web page,
    // and repeated large allocations fragment the heap TLS needs.
    std::vector<uint16_t> logo;
    logo.swap(_current.flight.airline_logo_rgb565);
    out = _current.flight;
    logo.swap(_current.flight.airline_logo_rgb565);
    return true;
}

void NeoMatrixDisplay::displayMessage(const String &message)
{
    if (_matrix == nullptr)
        return;
    if (_task)
    {
        Lock l(_lock);
        _message        = message;
        _messageUntilMs = millis() + kMessageMs;
        return;
    }
    renderMessage(message);
}

void NeoMatrixDisplay::showLoading()
{
    if (_task) return;
    BootSplash::render(g_workFrame, millis(), _runwayInUse);
    present();
}

// ── Display task ─────────────────────────────────────────────────────────────

uint32_t NeoMatrixDisplay::renderFrame(unsigned long now)
{
    applyPanelSettings();
    pollButton(now);

    if (_captionUntilMs && (long)(now - _captionUntilMs) < 0)
    {
        InfoScreens::renderCaption(g_workFrame, _caption);
        present();
        return kCardFrameMs;
    }
    _captionUntilMs = 0;

    if (renderSpriteGallery())
        return kCardFrameMs;

    if (s_splashRequestMs)
    {
        s_splashUntil     = now + s_splashRequestMs;
        s_splashRequestMs = 0;
    }
    if (s_splashUntil && (long)(now - s_splashUntil) < 0)
    {
        BootSplash::render(g_workFrame, now, _runwayInUse);
        present();
        return kCardFrameMs;
    }
    s_splashUntil = 0;

    if (s_panelMsgPending)
    {
        s_panelMsgPending = false;
        _message        = s_panelMsg;
        _messageUntilMs = now + kMessageMs;
    }
    if (_messageUntilMs && (long)(now - _messageUntilMs) < 0)
    {
        renderMessage(_message);
        return kCardFrameMs;
    }
    _messageUntilMs = 0;

    // Emergency squawk: takes over the panel for a few seconds.
    if (s_alertDemo)
    {
        s_alertDemo = false;
        strlcpy(_alertCode, "7700", sizeof(_alertCode));
        strlcpy(_alertMeaning, "EMERGENCY", sizeof(_alertMeaning));
        strlcpy(_alertIdent, "BA117 DEMO", sizeof(_alertIdent));
        strlcpy(_alertDetail, "4200FT 12KM E", sizeof(_alertDetail));
        _alertStartMs = now;
        _alertActive  = true;
    }
    if (_alertActive)
    {
        const uint32_t t = now - _alertStartMs;
        if (t < kAlertMs)
        {
            InfoScreens::renderAlert(g_workFrame, _alertCode, _alertMeaning, _alertIdent, _alertDetail, t);
            present();
            return kCardFrameMs;
        }
        _alertActive = false;
    }

    // Web demo: the rare-spot flourish, then a fly-across onto the current card.
    if (s_rareDemo && _hasCurrent && !_inTransition)
    {
        s_rareDemo = false;
        snprintf(_flourishLine1, sizeof(_flourishLine1), "RARE SPOT");
        snprintf(_flourishLine2, sizeof(_flourishLine2), "A380");
        _flourishActive  = true;
        _flourishStartMs = now;
        _inTransition    = true;
        _path = FlyAcross::pathFor(_current.flight.heading, _current.flight.vertical_rate,
                                   g_config.screen_facing);
    }
    s_rareDemo = false;

    if (s_replayRequest && _hasCurrent && !_inTransition)
    {
        const uint8_t req = s_replayRequest;
        s_replayRequest = 0;
        g_oldFrame.copyFrom(g_shownFrame);
        _inTransition = true;
        _transStartMs = now;
        _path = FlyAcross::pathFor(_current.flight.heading, _current.flight.vertical_rate,
                                   g_config.screen_facing);
        if (req == 2) _path.rightward = true;
        if (req == 3) _path.rightward = false;
    }
    s_replayRequest = 0;

    // Rare spot: a banner first, then the fly-across starts from it.
    if (_flourishActive)
    {
        const uint32_t t = now - _flourishStartMs;
        if (t < kFlourishMs)
        {
            InfoScreens::renderRareBanner(g_workFrame, _flourishLine1, _flourishLine2, t);
            present();
            return kAnimFrameMs;
        }
        _flourishActive = false;
        g_oldFrame.copyFrom(g_shownFrame);
        _transStartMs = now;
    }

    if (_inTransition)
    {
        const uint32_t t = now - _transStartMs;
        renderCurrentCard(now);
        if (t < FlyAcross::DURATION_MS)
        {
            const FlightInfo &f = _current.flight;
            bool known = false;
            const AircraftSprites::Kind kind = AircraftSprites::classify(f.aircraft_code, known);
            FlyAcross::compose(g_workFrame, g_oldFrame, t, AircraftSprites::get(kind),
                               _current.accent, !known, _path);
            present();
            return kAnimFrameMs;
        }
        _inTransition = false;
        _shownSinceMs = now;
        present();
        return kCardFrameMs;
    }

    if (_mode == Mode::Auto && renderLanding(now))
        return kAnimFrameMs;

    // ── What to show: the rhythm ────────────────────────────────────────────
    // The screen rotation (map, arrivals, stats, weather) is the resting
    // state; a card is an event. An approach card comes in when its landing is
    // within the lead time and stays until it has landed; a departure or
    // overflight gets a short card. After every landing comes a break of one
    // rotation screen before the next card, unless that one is about to land.
    const float leadSec = (float)g_config.card_lead_seconds;
    bool awaitingLanding = false;
    if (_hasCurrent && !_current.landingPlayed && !_current.goAround)
    {
        const ApproachStatus s = liveStatus(now);
        awaitingLanding = (s.phase == ApproachStatus::Approach || s.phase == ApproachStatus::Landing) &&
                          !isnan(s.etaSec) && s.etaSec <= leadSec;
    }

    // Landed: queue the next aircraft on approach, even one shown before; it
    // flies in once it is due.
    if (_hasCurrent && _current.landed && _queue.empty() && _hasNextApproach &&
        _nextApproach.flight.ident != _current.flight.ident)
    {
        _queue.push_back(_nextApproach);
        _hasNextApproach = false;
    }

    // In a break, or about to start one (a landed card whose break is still
    // to come): only an aircraft about to land may cut in.
    const bool breakPending = _hasCurrent && _current.landed && !_current.breakTaken &&
                              g_config.interlude_seconds > 0 && !_ambientActive;
    const bool interlude = (_ambientActive && (long)(now - _interludeUntilMs) < 0) || breakPending;
    unsigned long holdMs = kShortCardMs;
    if (_hasCurrent && _current.landed)       holdMs = kLandedHoldMs;
    else if (_hasCurrent && isArrival(_current)) holdMs = (unsigned long)g_config.display_cycle_seconds * 1000UL;
    const bool cardHeld = _hasCurrent && !_ambientActive && (awaitingLanding || now - _shownSinceMs < holdMs);

    if (_mode == Mode::Auto && !cardHeld)
    {
        const int i = dueIndex(now, interlude);
        if (i >= 0)
        {
            if (i > 0) std::rotate(_queue.begin(), _queue.begin() + i, _queue.begin() + i + 1);
            beginNextCard(now);
            return 0;   // draw the first animation frame straight away
        }
    }

    // A card whose flight has stopped appearing in fetches has left range.
    if (_hasCurrent && now - _current.dataMs > kIdleAfterMs)
    {
        Log.printf("Display: %s out of range\n", _current.flight.ident.c_str());
        _hasCurrent = false;
    }

    if (s_mapRequestMs)
    {
        s_mapPreviewUntil = now + s_mapRequestMs;
        s_mapRequestMs    = 0;
    }
    if (s_screenPreview && (long)(now - s_screenPreviewUntil) >= 0) s_screenPreview = 0;
    const bool preview = (s_mapPreviewUntil && (long)(now - s_mapPreviewUntil) < 0) || s_screenPreview;
    if (!preview) s_mapPreviewUntil = 0;

    // Back to the current approach card once its landing is due (during a
    // break, only if it is about to land).
    bool backToCard = false;
    if (_hasCurrent && awaitingLanding)
    {
        const float eta = entryEta(_current, now);
        backToCard = !interlude || (!isnan(eta) && eta <= kImminentSec);
    }

    bool showAmbient;
    if (!_hasCurrent || preview || _mode != Mode::Auto) showAmbient = true;
    else if (_ambientActive)                           showAmbient = !backToCard;
    else                                               showAmbient = !cardHeld;

    if (showAmbient && !_ambientActive)
    {
        g_oldFrame.copyFrom(g_shownFrame);   // cross-fade from whatever is showing
        _ambientActive  = true;
        _ambientSinceMs = now;
        _screenSinceMs  = now;
        // A break after a landing shows one screen; it is the next in the
        // rotation each time, so they all come round.
        const bool startBreak = _hasCurrent && _current.landed && !_current.breakTaken;
        if (startBreak) _current.breakTaken = true;
        _interludeUntilMs = startBreak ? now + (unsigned long)g_config.interlude_seconds * 1000UL : now;
        nextScreen();
        static const char *const kScreenNames[] = {"map", "arrivals", "stats", "weather"};
        if (!preview && _mode == Mode::Auto)
            Log.printf("Display: %s, %s\n", _interludeUntilMs != now ? "break after landing" : "screens",
                       kScreenNames[(int)_screen]);
    }
    else if (!showAmbient && _ambientActive)
    {
        // Back to the card with the usual fly-across.
        _ambientActive = false;
        g_oldFrame.copyFrom(g_shownFrame);
        _inTransition = true;
        _transStartMs = now;
        _path = FlyAcross::pathFor(_current.flight.heading, _current.flight.vertical_rate,
                                   g_config.screen_facing);
        return 0;
    }

    if (_ambientActive)
    {
        // Next screen in the rotation when this one has had its time.
        if (_mode == Mode::Auto && !preview && now - _screenSinceMs >= screenDwellMs(_screen))
        {
            g_oldFrame.copyFrom(g_shownFrame);
            _ambientSinceMs = now;
            _screenSinceMs  = now;
            nextScreen();
        }
        renderAmbient(now);
        const uint32_t t = now - _ambientSinceMs;
        if (t < kAmbientFadeMs)
        {
            crossFade(g_oldFrame, (float)t / kAmbientFadeMs);
            present();
            return kAnimFrameMs;
        }
        present();
        return kCardFrameMs;
    }

    renderCurrentCard(now);
    present();
    return kCardFrameMs;
}

// ── Rotation helpers ─────────────────────────────────────────────────────────

bool NeoMatrixDisplay::isArrival(const Entry &e) const
{
    const ApproachStatus s = ApproachModel::evaluate(e.flight);
    return s.phase == ApproachStatus::Approach || s.phase == ApproachStatus::Landing ||
           s.phase == ApproachStatus::Inbound;
}

// Seconds to touchdown, dead-reckoned; NAN unless on final.
float NeoMatrixDisplay::entryEta(const Entry &e, unsigned long now) const
{
    const ApproachStatus s = ApproachModel::advance(ApproachModel::evaluate(e.flight), e.flight.velocity,
                                                    now - e.dataMs);
    const bool onFinal = s.phase == ApproachStatus::Approach || s.phase == ApproachStatus::Landing;
    return onFinal ? s.etaSec : NAN;
}

// First queued flight that is due a card: an approach within the lead time,
// or any departure / overflight. Arrivals not yet on final wait (they are on
// the arrivals board). imminentOnly: just approaches about to land.
int NeoMatrixDisplay::dueIndex(unsigned long now, bool imminentOnly) const
{
    const float leadSec = (float)g_config.card_lead_seconds;
    for (size_t i = 0; i < _queue.size(); ++i)
    {
        const Entry &e = _queue[i];
        const float eta = entryEta(e, now);
        if (!isnan(eta))
        {
            if (eta <= (imminentOnly ? kImminentSec : leadSec)) return (int)i;
            continue;
        }
        if (!imminentOnly && !isArrival(e)) return (int)i;
    }
    return -1;
}

bool NeoMatrixDisplay::screenAvailable(Screen s) const
{
    if (!(g_config.screens & (1u << (uint8_t)s))) return false;
    switch (s)
    {
        case Screen::Map:      return !_traffic.empty();
        case Screen::Arrivals: return _arrivalCount > 0;
        case Screen::Weather:  return _metar.valid;
        default:               return true;
    }
}

void NeoMatrixDisplay::nextScreen()
{
    for (int k = 1; k <= (int)Screen::Count; ++k)
    {
        const Screen s = (Screen)(((int)_screen + k) % (int)Screen::Count);
        if (screenAvailable(s)) { _screen = s; return; }
    }
    _screen = Screen::Map;   // nothing enabled or available: the map / scanning screen
}

unsigned long NeoMatrixDisplay::screenDwellMs(Screen s) const
{
    return s == Screen::Map ? kMapDwellMs : kScreenDwellMs;
}

// The screen shown when no card is due.
void NeoMatrixDisplay::renderAmbient(unsigned long now)
{
    // Web previews, then the button-selected mode, then the rotation.
    Screen s = _screen;
    if      (s_screenPreview == 1) s = Screen::Stats;
    else if (s_screenPreview == 2) { InfoScreens::renderClock(g_workFrame, now, _weather); return; }
    else if (s_screenPreview == 3) s = Screen::Arrivals;
    else if (s_screenPreview == 4) s = Screen::Weather;
    else if (s_mapPreviewUntil)    s = Screen::Map;
    else if (_mode == Mode::Map)      s = Screen::Map;
    else if (_mode == Mode::Arrivals) s = Screen::Arrivals;
    else if (_mode == Mode::Stats)    s = Screen::Stats;
    else if (_mode == Mode::Weather)  s = Screen::Weather;
    else if (_traffic.empty())
    {
        // Nothing tracked: a dim clock overnight, otherwise the scanning screen.
        if (isNightActive()) InfoScreens::renderClock(g_workFrame, now, _weather);
        else                 BootSplash::render(g_workFrame, now, _runwayInUse);
        return;
    }

    switch (s)
    {
        case Screen::Arrivals:
            InfoScreens::renderArrivals(g_workFrame, _arrivals, _arrivalCount, _runwayInUse, _weather, now);
            return;
        case Screen::Stats:
            InfoScreens::renderStats(g_workFrame, _stats, now);
            return;
        case Screen::Weather:
            InfoScreens::renderWeather(g_workFrame, _metar, _runwayInUse, now);
            return;
        default:
            break;
    }
    const bool homeSet = g_config.home_lat != 0 || g_config.home_lon != 0;
    MapRenderer::render(g_workFrame, _traffic, now,
                        homeSet ? g_config.home_lat : g_config.center_lat,
                        homeSet ? g_config.home_lon : g_config.center_lon, _runwayInUse);
}

// Blend the work frame with `from`: k = 0 shows `from`, 1 shows the work frame.
void NeoMatrixDisplay::crossFade(const FrameCanvas &from, float k)
{
    uint16_t       *px  = g_workFrame.pixels();
    const uint16_t *old = from.pixels();
    for (int i = 0; i < FrameCanvas::W * FrameCanvas::H; ++i)
    {
        if (px[i] == old[i]) continue;
        const Rgb a = FrameCanvas::unpack(old[i]), b = FrameCanvas::unpack(px[i]);
        px[i] = FrameCanvas::pack(Rgb{ (uint8_t)(a.r + (b.r - a.r) * k), (uint8_t)(a.g + (b.g - a.g) * k),
                                       (uint8_t)(a.b + (b.b - a.b) * k) });
    }
}

void NeoMatrixDisplay::updateTraffic(const std::vector<TrafficPoint> &points)
{
    Lock l(_lock);
    _traffic.update(points, millis());
}

void NeoMatrixDisplay::beginNextCard(unsigned long now)
{
    g_oldFrame.copyFrom(g_shownFrame);   // exactly what is on the panel now (card or map)
    _ambientActive = false;
    _current    = _queue.front();
    _queue.pop_front();
    _hasCurrent = true;
    _inTransition = true;
    _transStartMs = now;
    _path = FlyAcross::pathFor(_current.flight.heading, _current.flight.vertical_rate,
                               g_config.screen_facing);
    const RareSpotter::Reason why = RareSpotter::check(_current.flight);
    if (why != RareSpotter::None)
    {
        RareSpotter::banner(why, _current.flight, _flourishLine1, sizeof(_flourishLine1),
                            _flourishLine2, sizeof(_flourishLine2));
        _flourishActive  = true;
        _flourishStartMs = now;
        Log.printf("Display: rare spot %s: %s %s\n", _current.flight.ident.c_str(), _flourishLine1, _flourishLine2);
    }
    // A departure just off the ground gets the take-off scene instead of the
    // fly-across (renderLanding starts it on the next frame).
    if (!_flourishActive && isFreshDeparture(_current.flight))
    {
        _inTransition = false;
        Log.printf("Display: take-off scene for %s (%u still queued)\n",
                   _current.flight.ident.c_str(), (unsigned)_queue.size());
        return;
    }
    Log.printf("Display: fly-across to %s, %s, %s (%u still queued)\n",
               _current.flight.ident.c_str(),
               _path.rightward ? "left-to-right" : "right-to-left",
               _path.endY > _path.startY ? "descending" : _path.endY < _path.startY ? "climbing" : "level",
               (unsigned)_queue.size());
}

ApproachStatus NeoMatrixDisplay::liveStatus(unsigned long now)
{
    const FlightInfo &f = _current.flight;
    ApproachStatus s = ApproachModel::advance(ApproachModel::evaluate(f), f.velocity, now - _current.dataMs);
    if (s.runway[0])
        strlcpy(_current.runway, s.runway, sizeof(_current.runway));
    else if (_current.landed)
        strlcpy(s.runway, _current.runway, sizeof(s.runway));
    return s;
}

void NeoMatrixDisplay::renderCurrentCard(unsigned long now)
{
    const FlightInfo &f = _current.flight;

    // Smoothed altitude: dead-reckon with the vertical rate, then ease the
    // shown value towards it, so new readings glide in instead of jumping.
    if (!isnan(f.baro_altitude))
    {
        double target = f.baro_altitude;
        if (!isnan(f.vertical_rate))
            target += f.vertical_rate * min(now - _current.dataMs, 60000UL) / 60000.0;
        if (isnan(_current.shownAltFt) || _current.altMs == 0)
            _current.shownAltFt = target;
        else
        {
            const float dt = (now - _current.altMs) / 1000.0f;
            _current.shownAltFt += (target - _current.shownAltFt) * (1.0f - expf(-dt / kAltEaseSec));
        }
        _current.altMs = now;
    }

    CardRenderer::Options o;
    o.animMs      = now;
    o.dataAgeMs   = now - _current.dataMs;
    o.border      = g_config.display_border;
    o.spriteRight = FlyAcross::pathFor(f.heading, f.vertical_rate, g_config.screen_facing).rightward;
    o.landed      = _current.landed;
    o.landedAt    = _current.landedAt;
    o.altFt       = _current.shownAltFt;
    o.goAround    = _current.goAround;
    CardRenderer::render(g_workFrame, f, liveStatus(now), _current.accent, o);
}

// Called with each new report for the card's flight: remembers when it was
// on final, spots a go-around (climbing away after being on final), and
// clears the go-around once it is back on approach and descending.
void NeoMatrixDisplay::noteApproachProgress(Entry &e, const ApproachStatus &st, double prevAlt,
                                            unsigned long now)
{
    const FlightInfo &f = e.flight;
    const bool onFinal = st.phase == ApproachStatus::Approach || st.phase == ApproachStatus::Landing;
    const bool descending = !isnan(f.vertical_rate) && f.vertical_rate < -200;

    if (e.goAround)
    {
        if (onFinal && descending && now - e.goAroundMs > 60000)
        {
            e.goAround = false;
            e.finalMs = now;
            Log.printf("Display: %s back on approach after its go-around\n", f.ident.c_str());
        }
        return;
    }
    const bool recentlyFinal = e.finalMs && now - e.finalMs < kFinalMemoryMs;
    const bool climbingAway = !isnan(f.vertical_rate) && f.vertical_rate > kGoAroundClimbFpm &&
                              !isnan(f.baro_altitude) && !isnan(prevAlt) &&
                              f.baro_altitude > prevAlt + 50 && f.baro_altitude < kGoAroundMaxFt;
    if (recentlyFinal && !e.landingPlayed && climbingAway)
    {
        e.goAround = true;
        e.goAroundMs = now;
        e.goAroundAnimPending = true;
        e.finalMs = 0;
        _stats.noteGoAround();
        Log.printf("Display: %s GO-AROUND (%.0f ft, +%.0f fpm)\n", f.ident.c_str(),
                   f.baro_altitude, f.vertical_rate);
        return;
    }
    if (onFinal && descending) e.finalMs = now;
}

void NeoMatrixDisplay::startScene(unsigned long now, LandingScene::Kind kind, bool demo)
{
    _sceneKind      = kind;
    _landingDemo    = demo;
    _landingActive  = true;
    _landingStartMs = now;
}

void NeoMatrixDisplay::startLanding(unsigned long now, bool demo)
{
    if (!demo)
    {
        _current.landingPlayed = true;
        const time_t t = time(nullptr);
        _current.landedAt = t > 1600000000 ? t : 0;   // only if the clock has synced
    }
    startScene(now, LandingScene::Landing, demo);
    Log.printf("Display: %s touchdown on %s\n", _current.flight.ident.c_str(),
               _current.runway[0] ? _current.runway : "runway");
}

// Returns true while the landing animation owns the panel.
bool NeoMatrixDisplay::renderLanding(unsigned long now)
{
    if (s_landingReplay)
    {
        s_landingReplay = false;
        if (_hasCurrent && !_inTransition) startLanding(now, true);
    }

    if (s_goAroundDemo)
    {
        s_goAroundDemo = false;
        if (_hasCurrent && !_inTransition && !_landingActive)
            startScene(now, LandingScene::GoAround, true);
    }
    if (s_takeoffDemo)
    {
        s_takeoffDemo = false;
        if (_hasCurrent && !_inTransition && !_landingActive)
        {
            liveStatus(now);   // picks up the departure runway if there is one
            startScene(now, LandingScene::Takeoff, true);
        }
    }

    // A go-around just detected: play its animation once.
    if (_hasCurrent && !_inTransition && !_landingActive && _current.goAroundAnimPending)
    {
        _current.goAroundAnimPending = false;
        startScene(now, LandingScene::GoAround, false);
    }

    // A departure just off the ground: the take-off scene, once.
    if (_hasCurrent && !_inTransition && !_landingActive && !_current.takeoffPlayed &&
        isFreshDeparture(_current.flight))
    {
        _current.takeoffPlayed = true;
        liveStatus(now);
        startScene(now, LandingScene::Takeoff, false);
        Log.printf("Display: %s take-off from %s\n", _current.flight.ident.c_str(),
                   _current.runway[0] ? _current.runway : "runway");
    }

    // Touchdown, confirmed by the data rather than predicted: at the threshold
    // by dead reckoning, and either reported low (< 200 ft) near it, or gone
    // from the data for a while (on the ground, under the altitude filter).
    if (_hasCurrent && !_inTransition && !_landingActive && !_current.landingPlayed && !_current.goAround)
    {
        const ApproachStatus s = liveStatus(now);
        const FlightInfo &f = _current.flight;
        const bool atThreshold = s.phase == ApproachStatus::Landing ||
                                 (s.phase == ApproachStatus::Approach && !isnan(s.distKm) && s.distKm <= 0.05f);
        const bool reportedLow = !isnan(f.baro_altitude) && f.baro_altitude < kLandedReportFt &&
                                 (isnan(f.vertical_rate) || f.vertical_rate < 100) &&
                                 !isnan(s.distKm) && s.distKm < 1.5f;
        const bool silent = now - _current.dataMs > kLandedSilentMs;
        if (atThreshold && (reportedLow || silent))
            startLanding(now, false);
    }

    if (!_landingActive) return false;
    const uint32_t t = now - _landingStartMs;
    if (t >= LandingScene::DURATION_MS)
    {
        _landingActive  = false;
        if (!_landingDemo && _sceneKind == LandingScene::Landing)
            _current.landed = true;
        if (!_landingDemo && _sceneKind != LandingScene::GoAround)
            _shownSinceMs = now;   // give the card that follows its full hold time
        _sceneKind = LandingScene::Landing;
        return false;
    }
    const FlightInfo &f = _current.flight;
    bool known = false;
    const AircraftSprites::Kind kind = AircraftSprites::classify(f.aircraft_code, known);
    const bool rightward = FlyAcross::pathFor(f.heading, f.vertical_rate, g_config.screen_facing).rightward;
    // Second caption line: the flight, plus where it is going for a departure.
    static char caption[24];
    const String &id = f.ident_iata.length() ? f.ident_iata : f.ident;
    const String &dest = f.destination.code_iata.length() ? f.destination.code_iata : f.destination.code_icao;
    if (_sceneKind == LandingScene::Takeoff && dest.length() && !AirportPack::isHome(f.destination))
        snprintf(caption, sizeof(caption), "%s TO %s", id.c_str(), dest.c_str());
    else
        snprintf(caption, sizeof(caption), "%s", id.c_str());
    LandingScene::render(g_workFrame, t, AircraftSprites::get(kind), _current.accent, !known,
                         rightward, caption, _current.runway, _sceneKind);
    present();
    return true;
}

void NeoMatrixDisplay::renderMessage(const String &message)
{
    g_workFrame.clear();
    const String msg = message.substring(0, FrameCanvas::W / 6);
    g_workFrame.text((FrameCanvas::W - FrameCanvas::textWidth(msg)) / 2,
                     (FrameCanvas::H - 7) / 2, msg, Rgb{235, 240, 245});
    present();
}

void NeoMatrixDisplay::applyPanelSettings()
{
    const uint8_t targetBrightness = effectiveBrightness();
    if (targetBrightness != _lastBrightness)
    {
        _lastBrightness = targetBrightness;
        _matrix->setBrightness8(targetBrightness);
    }
    if (g_config.display_flip != _lastFlip)
    {
        _lastFlip = g_config.display_flip;
        _matrix->setRotation(g_config.display_flip ? 2 : 0);
        _forceFull = true;
    }
}

// The board's button cycles Auto -> Map -> Arrivals -> Stats -> Weather -> Auto.
void NeoMatrixDisplay::pollButton(unsigned long now)
{
    const bool down = digitalRead(kButtonPin) == LOW;
    if (down && !_buttonDown && now - _buttonChangeMs > 60)
    {
        static const char *const kNames[] = {"AUTO", "MAP", "ARRIVALS", "STATS", "WEATHER"};
        _mode = (Mode)(((uint8_t)_mode + 1) % 5);
        snprintf(_caption, sizeof(_caption), "%s", kNames[(uint8_t)_mode]);
        _captionUntilMs = now + kCaptionMs;
        Log.printf("Display: button -> %s mode\n", _caption);
    }
    if (down != _buttonDown) _buttonChangeMs = now;
    _buttonDown = down;
}

void NeoMatrixDisplay::present()
{
    StageTrace::mark(StageTrace::Display, StageTrace::DispPresent);
    const uint16_t *next  = g_workFrame.pixels();
    uint16_t       *shown = g_shownFrame.pixels();
    for (int i = 0; i < FrameCanvas::W * FrameCanvas::H; ++i)
    {
        if (!_forceFull && next[i] == shown[i]) continue;
        shown[i] = next[i];
        _matrix->drawPixel(i % FrameCanvas::W, i / FrameCanvas::W, next[i]);
    }
    _forceFull = false;
}

// 3x3 grid of every aircraft sprite, tails in the default accent.
bool NeoMatrixDisplay::renderSpriteGallery()
{
    const unsigned long now = millis();
    if (s_galleryRequestMs)
    {
        s_galleryUntil     = now + s_galleryRequestMs;
        s_galleryRequestMs = 0;
    }
    if (!s_galleryUntil) return false;
    if ((long)(now - s_galleryUntil) >= 0)
    {
        s_galleryUntil = 0;
        return false;
    }
    g_workFrame.clear();
    for (int k = 0; k < AircraftSprites::KindCount; ++k)
    {
        const AircraftSprites::Sprite &sp = AircraftSprites::get((AircraftSprites::Kind)k);
        const int cx = (k % 3) * 43, cy = (k / 3) * 21;
        AircraftSprites::draw(g_workFrame, sp, cx + (42 - sp.w) / 2, cy + (21 - sp.h) / 2,
                              Rgb{90, 170, 255});
    }
    present();
    return true;
}
