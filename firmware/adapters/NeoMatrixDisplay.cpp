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
static constexpr float         kHoldForLandingSec = 300.0f;          // keep an approach card if ETA below this
static constexpr unsigned long kLandedHoldMs    = 10000;             // LANDED card, then the next approach
static constexpr float         kAltEaseSec      = 0.8f;              // altitude smoothing time constant
static constexpr unsigned long kCardBeforeMapMs = 75000;             // card up this long with nothing due -> map
static constexpr unsigned long kMapShowMs       = 30000;             // then the map for this long
static constexpr uint32_t      kAmbientFadeMs   = 700;               // card -> map cross-fade
static constexpr float         kBackForLandingSec = 90.0f;           // leave the map for a landing this close
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

void requestSpriteGallery(uint32_t durationMs) { s_galleryRequestMs = durationMs ? durationMs : 1; }

// Replay request from the web task: 0 none, 1 real path, 2 force L→R, 3 force R→L.
static volatile uint8_t s_replayRequest = 0;

static volatile bool s_landingReplay = false;

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
            _current.flight = f;
            _current.dataMs = now;
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

    if (_messageUntilMs && (long)(now - _messageUntilMs) < 0)
    {
        renderMessage(_message);
        return kCardFrameMs;
    }
    _messageUntilMs = 0;

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

    if (renderLanding(now))
        return kAnimFrameMs;

    // A plane on final approach keeps the card until it has landed, so new
    // contacts wait rather than cutting off the landing.
    bool awaitingLanding = false, landingSoon = false;
    if (_hasCurrent && !_current.landingPlayed)
    {
        const ApproachStatus s = liveStatus(now);
        awaitingLanding = s.phase == ApproachStatus::Approach &&
                          !isnan(s.etaSec) && s.etaSec <= kHoldForLandingSec;
        landingSoon = awaitingLanding && s.etaSec <= kBackForLandingSec;
    }

    // Landed: after ~10 s move on to the next aircraft on approach, even one
    // that has been on the card before.
    if (_hasCurrent && _current.landed && _queue.empty() && _hasNextApproach &&
        _nextApproach.flight.ident != _current.flight.ident)
    {
        _queue.push_back(_nextApproach);
        _hasNextApproach = false;
    }

    const unsigned long holdMs = _hasCurrent && _current.landed
        ? kLandedHoldMs
        : (unsigned long)g_config.display_cycle_seconds * 1000UL;
    if (!_queue.empty() && (!_hasCurrent || (now - _shownSinceMs >= holdMs && !awaitingLanding)))
    {
        beginNextCard(now);
        return 0;   // draw the first animation frame straight away
    }

    // A card whose flight has stopped appearing in fetches has left range.
    if (_hasCurrent && now - _current.dataMs > kIdleAfterMs)
    {
        Log.printf("Display: %s out of range\n", _current.flight.ident.c_str());
        _hasCurrent = false;
    }

    // Ambient screen (the map) when no card is due: nothing on the card, a
    // card that has been up a while with nothing else queued, or a landed
    // card past its hold. Back to the card for its landing.
    if (s_mapRequestMs)
    {
        s_mapPreviewUntil = now + s_mapRequestMs;
        s_mapRequestMs    = 0;
    }
    const bool mapPreview = s_mapPreviewUntil && (long)(now - s_mapPreviewUntil) < 0;
    if (!mapPreview) s_mapPreviewUntil = 0;

    bool showAmbient;
    if (!_hasCurrent || mapPreview)
        showAmbient = true;
    else if (_ambientActive)
        showAmbient = !landingSoon && (_current.landed || (long)(now - _ambientUntilMs) < 0);
    else
        showAmbient = !awaitingLanding && _queue.empty() &&
                      now - _shownSinceMs >= (_current.landed ? kLandedHoldMs : kCardBeforeMapMs);

    if (showAmbient && !_ambientActive)
    {
        g_oldFrame.copyFrom(g_shownFrame);   // cross-fade from whatever is showing
        _ambientActive  = true;
        _ambientSinceMs = now;
        _ambientUntilMs = now + kMapShowMs;
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

// The screen shown when no card is due: the map while aircraft are being
// tracked, otherwise the scanning screen.
void NeoMatrixDisplay::renderAmbient(unsigned long now)
{
    if (_traffic.empty())
    {
        BootSplash::render(g_workFrame, now, _runwayInUse);
        return;
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
    CardRenderer::render(g_workFrame, f, liveStatus(now), _current.accent, o);
}

void NeoMatrixDisplay::startLanding(unsigned long now, bool demo)
{
    _landingDemo = demo;
    if (!demo)
    {
        _current.landingPlayed = true;
        const time_t t = time(nullptr);
        _current.landedAt = t > 1600000000 ? t : 0;   // only if the clock has synced
    }
    _landingActive  = true;
    _landingStartMs = now;
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

    // Trigger once, when the dead-reckoned approach reaches the threshold.
    if (_hasCurrent && !_inTransition && !_landingActive && !_current.landingPlayed)
    {
        const ApproachStatus s = liveStatus(now);
        if (s.phase == ApproachStatus::Landing ||
            (s.phase == ApproachStatus::Approach && !isnan(s.distKm) && s.distKm <= 0.05f))
            startLanding(now, false);
    }

    if (!_landingActive) return false;
    const uint32_t t = now - _landingStartMs;
    if (t >= LandingScene::DURATION_MS)
    {
        _landingActive  = false;
        if (!_landingDemo)
        {
            _current.landed = true;
            _shownSinceMs   = now;   // give the LANDED card its full hold time
        }
        return false;
    }
    const FlightInfo &f = _current.flight;
    bool known = false;
    const AircraftSprites::Kind kind = AircraftSprites::classify(f.aircraft_code, known);
    const bool rightward = FlyAcross::pathFor(f.heading, f.vertical_rate, g_config.screen_facing).rightward;
    LandingScene::render(g_workFrame, t, AircraftSprites::get(kind), _current.accent, !known,
                         rightward, f.ident, _current.runway);
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
