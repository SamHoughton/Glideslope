/*
Purpose: The display's rhythm: what is on the panel each frame (alerts and
scenes first, then the card or the rotation), the rotation of screens, and
the pages on final. Part of NeoMatrixDisplay (see NeoMatrixDisplay.cpp).
*/
#include "adapters/DisplayInternal.h"
#include "config/Airports.h"
#include "display/LineupScreen.h"
#include "display/NightWatch.h"

bool NeoMatrixDisplay::departuresPage() const
{
    return (g_config.screens & 32) && _departureCount > 0;
}

// One frame: housekeeping, then whatever has the panel (alerts, previews,
// the overhead pass, the rare-spot banner), the fly-across, a landing
// scene, and otherwise the rhythm of cards and screens. The landing check
// comes before the rhythm every frame, so nothing there can hold it up.
uint32_t NeoMatrixDisplay::renderFrame(unsigned long now)
{
    applyPanelSettings();
    if (!_skyMs || now - _skyMs > 30000)
    {
        _sky = Sky::at(_metar, time(nullptr));
        _skyMs = now;
    }
    if (s_skyPreviewUntil && (long)(now - s_skyPreviewUntil) >= 0) s_skyPreviewUntil = 0;
    if (s_skyDemo)
    {
        // Replay the card's landing in the previewed sky, or the showcase if no card.
        s_skyDemo = false;
        if (_hasCurrent && !_inTransition && !_landingActive) requestLandingReplay();
        else                                                  s_showcaseRequest = true;
    }
    pollButton(now);

    uint32_t wait;
    if (renderOverlays(now, wait)) return wait;
    if (renderTransition(now, wait)) return wait;
    if (_mode == Mode::Auto && renderLanding(now))
        return kAnimFrameMs;

    return renderRhythm(now);
}

// Whatever takes over the panel before the card: true (with the wait until
// the next frame) when it drew the frame.
bool NeoMatrixDisplay::renderOverlays(unsigned long now, uint32_t &wait)
{
    if (_captionUntilMs && (long)(now - _captionUntilMs) < 0)
    {
        InfoScreens::renderCaption(g_workFrame, _caption);
        present();
        { wait = kCardFrameMs; return true; }
    }
    _captionUntilMs = 0;

    if (renderSpriteGallery())
        { wait = kCardFrameMs; return true; }

    if (s_splashRequestMs)
    {
        s_splashUntil     = now + s_splashRequestMs;
        s_splashRequestMs = 0;
    }
    if (s_splashUntil && (long)(now - s_splashUntil) < 0)
    {
        BootSplash::render(g_workFrame, now, _runwayInUse);
        present();
        { wait = kCardFrameMs; return true; }
    }
    s_splashUntil = 0;

    if (s_panelMsgPending)
    {
        s_panelMsgPending = false;
        _message        = s_panelMsg;
        _messageUntilMs = now + s_panelMsgMs;
    }
    if (_messageUntilMs && (long)(now - _messageUntilMs) < 0)
    {
        renderMessage(_message);
        { wait = kCardFrameMs; return true; }
    }
    _messageUntilMs = 0;

    if (s_showcaseRequest)
    {
        s_showcaseRequest = false;
        startShowcase(now);
        { wait = 0; return true; }
    }
    if (_showcaseActive) stepShowcase(now);

    // Emergency squawk: takes over the panel for a few seconds.
    if (s_alertDemo)
    {
        s_alertDemo = false;
        strlcpy(_alertCode, "7700", sizeof(_alertCode));
        strlcpy(_alertMeaning, "EMERGENCY", sizeof(_alertMeaning));
        strlcpy(_alertIdent, "GS101 DEMO", sizeof(_alertIdent));
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
            { wait = kCardFrameMs; return true; }
        }
        _alertActive = false;
    }

    // A go-around anywhere in range (8.3): its own alert, once a landing or
    // transition has finished (and not two minutes late). Before night
    // watch: rare enough at night to be worth the light.
    if (s_gaAlertDemo)
    {
        s_gaAlertDemo = false;
        strlcpy(_gaIdent, "GS101", sizeof(_gaIdent));
        strlcpy(_gaRwy, _runwayInUse[0] ? _runwayInUse : "27L", sizeof(_gaRwy));
        _gaAlt = 1200;
        _gaQueuedMs = now;
        _gaPending = true;
    }
    if (_gaPending && now - _gaQueuedMs > 120000UL)
    {
        _gaPending = false;
        Log.printf("Display: go-around alert for %s dropped (the panel was busy for 2 min)\n", _gaIdent);
    }
    if (_gaPending && !_landingActive && !_inTransition)
    {
        _gaPending = false;
        _gaActive = true;
        _gaMs = now;
        Log.printf("Display: go-around alert for %s%s%s (%d ft)\n", _gaIdent, _gaRwy[0] ? " off " : "", _gaRwy, _gaAlt);
    }
    if (_gaActive)
    {
        const uint32_t t = now - _gaMs;
        if (t < 10000)
        {
            InfoScreens::renderGoAroundAlert(g_workFrame, _gaIdent, _gaRwy, _gaAlt, t);
            present();
            { wait = kCardFrameMs; return true; }
        }
        _gaActive = false;
        g_oldFrame.copyFrom(g_shownFrame);
        _ambientSinceMs = now;
    }

    // Night watch: through the night hours just the quiet view (emergencies
    // above still take over); cards, scenes, banners and the rotation wait
    // for the morning.
    if (nightWatchNow(now))
    {
        renderNightWatch(now);
        present();
        { wait = kCardFrameMs; return true; }
    }
    _nwBase = -1;

    // Logbook news (a record, a milestone): a short gold banner, when no
    // landing or transition has the panel. The airfield's mood (8.2) the
    // same way, unless it has waited ten minutes (it is old news by then).
    if (_newsPending && _newsStyle && now - _newsQueuedMs > 600000UL) _newsPending = false;
    if (s_moodDemo)
    {
        _newsStyle = s_moodDemo;
        s_moodDemo = 0;
        if (_newsStyle == 5)
        {
            snprintf(_news1, sizeof(_news1), "New sticker");
            snprintf(_news2, sizeof(_news2), "Air Astana");
        }
        else if (_newsStyle == 4)
        {
            snprintf(_news1, sizeof(_news1), "Special livery");
            snprintf(_news2, sizeof(_news2), "Pride");
        }
        else if (_newsStyle == 3)
        {
            // The other end of the runway in use (09 at Heathrow on westerlies).
            const int now27 = atoi(_runwayInUse[0] ? _runwayInUse : "27");
            snprintf(_news1, sizeof(_news1), "Runway change due");
            snprintf(_news2, sizeof(_news2), "To %02d at 19:00", (now27 + 17) % 36 + 1);
        }
        else
        {
            Metar m;
            m.visM = 300;
            strlcpy(m.wx, "FG", sizeof(m.wx));
            char l1[24], l2[28];
            Weather::moodLines(_newsStyle == 1 ? Weather::LowVis : Weather::Storm, m, l1, sizeof(l1), l2, sizeof(l2));
            strlcpy(_news1, l1, sizeof(_news1));
            strlcpy(_news2, l2, sizeof(_news2));
        }
        _newsQueuedMs = now;
        _newsPending = true;
    }
    if (_newsPending && !_landingActive && !_inTransition && !_alertActive)
    {
        _newsPending = false;
        _newsActive = true;
        _newsMs = now;
        g_oldFrame.copyFrom(g_shownFrame);
    }
    if (_newsActive)
    {
        const uint32_t t = now - _newsMs;
        if (t < 6000)
        {
            if (_newsStyle) InfoScreens::renderMoodBanner(g_workFrame, _newsStyle, _news1, _news2, t);
            else            InfoScreens::renderRareBanner(g_workFrame, _news1, _news2, t);
            present();
            { wait = kCardFrameMs; return true; }
        }
        _newsActive = false;
        g_oldFrame.copyFrom(g_shownFrame);
        _ambientSinceMs = now;
    }

    // The landing direction has changed: announce it once.
    if (s_runwayDemo)
    {
        s_runwayDemo = false;
        const bool west = !_runwayInUse[0] || _runwayInUse[0] == '2';
        strlcpy(_rwyFrom, west ? "27R" : "09L", sizeof(_rwyFrom));
        strlcpy(_rwyTo, west ? "09L" : "27R", sizeof(_rwyTo));
        _rwyChangeMs = now;
        _rwyChangeActive = true;
    }
    if (_rwyChangeActive)
    {
        const uint32_t t = now - _rwyChangeMs;
        if (t < kRunwayChangeMs)
        {
            InfoScreens::renderRunwayChange(g_workFrame, _rwyFrom, _rwyTo, t);
            present();
            { wait = kCardFrameMs; return true; }
        }
        _rwyChangeActive = false;
    }

    // Overhead or watchlist: the pass across the sky, never over a landing.
    if (s_spotDemo)
    {
        s_spotDemo = false;
        OverheadScene::Info d;
        strlcpy(d.ident, "EK2", sizeof(d.ident));
        strlcpy(d.type, "A380", sizeof(d.type));
        strlcpy(d.where, g_config.route_cities ? "To Dubai" : "To DXB", sizeof(d.where));
        d.altFt = 36000;
        d.kind = AircraftSprites::A380;
        d.livery = AircraftSprites::Livery(Rgb{200, 30, 40});
        d.accent = Rgb{230, 50, 60};
        d.rightward = FlyAcross::pathFor(90, 0, g_config.screen_facing).rightward;
        _spot = d;
        _spotPending = true;
    }
    if (_spotPending && !_landingActive && !_alertActive)
    {
        _spotPending = false;
        _spotActive = true;
        _spotStartMs = now;
    }
    if (_spotActive)
    {
        const uint32_t t = now - _spotStartMs;
        if (t < OverheadScene::DURATION_MS)
        {
            OverheadScene::render(g_workFrame, _spot, t, s_skyPreviewUntil ? Sky::preview(s_skyPreview) : _sky);
            present();
            { wait = kAnimFrameMs; return true; }
        }
        _spotActive = false;
        g_oldFrame.copyFrom(g_shownFrame);   // fade back into whatever comes next
        _ambientSinceMs = now;
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
            { wait = kAnimFrameMs; return true; }
        }
        _flourishActive = false;
        g_oldFrame.copyFrom(g_shownFrame);
        _transStartMs = now;
    }
    return false;
}

// The fly-across onto a new card.
bool NeoMatrixDisplay::renderTransition(unsigned long now, uint32_t &wait)
{
    // From the map: the map closes in on the aircraft's icon (0-700 ms), then
    // the card fades in out of it (to 1,100 ms). One pass per pixel, sampling
    // the map (g_oldFrame) at the zoomed scale, blended with the card.
    if (_inTransition && _zoomFromMap && !_flourishActive)
    {
        constexpr uint32_t kZoomMs = 700, kFadeMs = 400;
        const uint32_t t = now - _transStartMs;
        if (t < kZoomMs + kFadeMs)
        {
            renderCurrentCard(now);
            const float u = min(1.0f, (float)t / kZoomMs);
            const float zoom = 1 + 5 * u * u * (3 - 2 * u);   // smoothstep to 6x
            const float k = t < kZoomMs ? 0 : (float)(t - kZoomMs) / kFadeMs;   // card's share
            // The icon stays put at first and drifts to the middle as it closes
            // in (sx = icon + (x - anchor) / zoom; the anchor moves from the
            // icon to the centre), so nothing jumps at the start.
            const float ax = _zoomX + (FrameCanvas::W / 2.0f - _zoomX) * u;
            const float ay = _zoomY + (FrameCanvas::H / 2.0f - _zoomY) * u;
            uint16_t *px = g_workFrame.pixels();
            for (int y = 0; y < FrameCanvas::H; ++y)
                for (int x = 0; x < FrameCanvas::W; ++x)
                {
                    const int sx = (int)lroundf(_zoomX + (x - ax) / zoom);
                    const int sy = (int)lroundf(_zoomY + (y - ay) / zoom);
                    const bool in = sx >= 0 && sy >= 0 && sx < FrameCanvas::W && sy < FrameCanvas::H;
                    Rgb m = in ? FrameCanvas::unpack(g_oldFrame.get(sy * FrameCanvas::W + sx)) : Rgb{0, 0, 0};
                    m = FrameCanvas::scale(m, 1.0f - 0.35f * u);   // the map dims as it closes in
                    const int i = y * FrameCanvas::W + x;
                    if (k <= 0) { px[i] = FrameCanvas::pack(m); continue; }
                    const Rgb cd = FrameCanvas::unpack(px[i]);
                    px[i] = FrameCanvas::pack(Rgb{(uint8_t)(m.r + (cd.r - m.r) * k), (uint8_t)(m.g + (cd.g - m.g) * k),
                                                  (uint8_t)(m.b + (cd.b - m.b) * k)});
                }
            present();
            wait = kAnimFrameMs;
            return true;
        }
        _zoomFromMap = false;
        _inTransition = false;
        _shownSinceMs = now;
        present();
        wait = kCardFrameMs;
        return true;
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
                               AircraftSprites::liveryFor(f, _current.accent), !known, _path);
            present();
            { wait = kAnimFrameMs; return true; }
        }
        _inTransition = false;
        _shownSinceMs = now;
        present();
        { wait = kCardFrameMs; return true; }
    }
    return false;
}

uint32_t NeoMatrixDisplay::renderRhythm(unsigned long now)
{
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

    if (_mode == Mode::Auto && !cardHeld && !_showcaseActive)
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
        // One name for every screen (8.0's line-up was missing: an index past the end crashed the display).
        static const char *const kScreenNames[] = {"map", "arrivals", "stats", "weather", "holding", "-", "globe",
                                                   "-", "line-up"};
        static_assert(sizeof(kScreenNames) / sizeof(kScreenNames[0]) == (size_t)Screen::Count, "a name for every screen");
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

    renderHeld(now);
    if (now - _storyFadeMs < kAmbientFadeMs)
    {
        crossFade(g_oldFrame, (float)(now - _storyFadeMs) / kAmbientFadeMs);
        present();
        return kAnimFrameMs;
    }
    present();
    return kCardFrameMs;
}

// Which page, and how long it has been up. From when the card came on: the
// card 12 s, then a page 7 s, the pages taking turns (the route globe, the
// approach line-up when someone else is on final too, the aircraft). A page
// only starts if it will be over 40 s before touchdown.
uint8_t NeoMatrixDisplay::storyPage(unsigned long now, uint32_t &pageMs)
{
    pageMs = 0;
    if (!(g_config.screens & 128) || _mode != Mode::Auto || _showcaseActive || !_hasCurrent ||
        _current.landed || _current.goAround || !isArrival(_current))
        return 0;
    const float eta = entryEta(_current, now);
    if (isnan(eta)) return 0;
    constexpr uint32_t kCard = 12000, kPage = 7000, kSlot = kCard + kPage;
    constexpr float kCutoffSec = 40;
    uint8_t pages[4];
    int np = 0;
    if (_globe.valid && strcmp(_globe.ident, (_current.flight.ident_iata.length() ?
                                              _current.flight.ident_iata : _current.flight.ident).c_str()) == 0)
        pages[np++] = 1;
    if (g_config.screens & 0x100)
    {
        LineupScreen::Plane p[LineupScreen::kMax];
        if (LineupScreen::collect(_traffic, now, p) >= 2) pages[np++] = 3;
    }
    {
        // Where it came from: an origin far enough away to be worth it.
        Airports::Place from;
        if (Airports::find(_current.flight.origin, from) && strcmp(from.icao, g_airport.icao) != 0) pages[np++] = 4;
    }
    pages[np++] = 2;
    const uint32_t t = now - _shownSinceMs;
    const uint32_t slot = t / kSlot, u = t % kSlot;
    if (u < kCard) return 0;
    const uint8_t page = pages[slot % np];
    const uint32_t len = kPage;
    pageMs = u - kCard;
    if (eta - (len - pageMs) / 1000.0f < kCutoffSec && pageMs < 400) return 0;   // too late to start one
    if (eta < kCutoffSec) return 0;
    return page;
}

void NeoMatrixDisplay::renderHeld(unsigned long now)
{
    renderCurrentCard(now);   // always: it keeps the altitude easing along
    uint32_t pageMs = 0;
    const uint8_t page = storyPage(now, pageMs);
    if (page != _storyPage)
    {
        g_oldFrame.copyFrom(g_shownFrame);   // cross-fade between the pages
        _storyFadeMs = now;
        _storyPage = page;
    }
    if (page == 1)
    {
        refreshGlobe(now);
        Globe::render(g_workFrame, _globe, pageMs + 1600, time(nullptr));   // already turned: no spin-in
    }
    else if (page == 2)
    {
        const float eta = entryEta(_current, now);
        InfoScreens::renderAircraft(g_workFrame, _current.flight, _current.accent, isnan(eta) ? -1 : eta,
                                    _current.runway, pageMs);
    }
    else if (page == 4)
    {
        const float eta = entryEta(_current, now);
        Metar wx;
        const String &code = _current.flight.origin.code_icao;
        const bool have = code.length() && originMetar(code.c_str(), wx);
        if (!InfoScreens::renderOrigin(g_workFrame, _current.flight, have ? &wx : nullptr, isnan(eta) ? -1 : eta,
                                       _current.runway, pageMs))
            InfoScreens::renderAircraft(g_workFrame, _current.flight, _current.accent, isnan(eta) ? -1 : eta,
                                        _current.runway, pageMs);
    }
    else if (page == 3)
    {
        LineupScreen::Plane p[LineupScreen::kMax];
        const int n = LineupScreen::collect(_traffic, now, p);
        LineupScreen::render(g_workFrame, p, n, pageMs, _sky);
    }
}

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
        case Screen::Arrivals: return _arrivalCount > 0 || departuresPage();
        case Screen::Weather:  return _metar.valid;
        case Screen::Holding:  return _holds.total(millis()) > 0;
        case Screen::Departures: return false;   // a page of the arrivals board
        case Screen::Globe:    return _globe.valid;
        case Screen::FinalPages: return false;
        case Screen::Lineup:
        {
            LineupScreen::Plane p[LineupScreen::kMax];
            return LineupScreen::collect(_traffic, millis(), p) >= 2;
        }
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
    if (s == Screen::Arrivals && departuresPage())
    {
        // Arrivals, then departures, then where they are heading.
        const int pages = (_arrivalCount > 0) + 1 + (InfoScreens::departureHeadings(_departures, _departureCount) >= 2);
        return pages * (kScreenDwellMs - 1000);
    }
    if (s == Screen::Stats) return kScreenDwellMs + (statsPages() - 1) * kStatsPageMs;
    return s == Screen::Map ? kMapDwellMs : kScreenDwellMs;
}

// How long screen s has been on the panel (restarts whenever it changes,
// previews and button modes included), for entry animations.
uint32_t NeoMatrixDisplay::shownFor(Screen s, unsigned long now)
{
    if (s != _shownScreen || now - _shownScreenLastMs > 1000)
    {
        _shownScreen = s;
        _shownScreenMs = now;
    }
    _shownScreenLastMs = now;
    return now - _shownScreenMs;
}

// The screen shown when no card is due.
void NeoMatrixDisplay::renderAmbient(unsigned long now)
{
    // Web previews, then the button-selected mode, then the rotation.
    Screen s = _screen;
    if      (s_screenPreview == 1) s = Screen::Stats;
    else if (s_screenPreview == 2) { InfoScreens::renderNightScene(g_workFrame, now, _weather, s_skyPreviewUntil ? Sky::preview(s_skyPreview) : _sky); return; }
    else if (s_screenPreview == 7) { InfoScreens::renderDaySummary(g_workFrame, _stats, shownFor(Screen::Map, now)); return; }
    else if (s_screenPreview == 10)
    {
        // The year (9.0): its five pages, four seconds each.
        const uint32_t t = shownFor(Screen::Map, now);
        InfoScreens::renderYear(g_workFrame, _year, (int)(t / 4000) % 5, t % 4000);
        return;
    }
    else if (s_screenPreview == 3) s = Screen::Arrivals;
    else if (s_screenPreview == 4) s = Screen::Weather;
    else if (s_screenPreview == 5) s = Screen::Holding;
    else if (s_screenPreview == 6) s = Screen::Globe;
    else if (s_screenPreview == 8)
    {
        LineupScreen::Plane p[LineupScreen::kMax];
        int n = LineupScreen::collect(_traffic, now, p);
        if (n < 2) n = LineupScreen::demo(p);   // nobody on final: a busy morning
        LineupScreen::render(g_workFrame, p, n, shownFor(Screen::Lineup, now),
                             s_skyPreviewUntil ? Sky::preview(s_skyPreview) : _sky);
        return;
    }
    else if (s_mapPreviewUntil)    s = Screen::Map;
    else if (_mode == Mode::Map)      s = Screen::Map;
    else if (_mode == Mode::Arrivals) s = Screen::Arrivals;
    else if (_mode == Mode::Stats)    s = Screen::Stats;
    else if (_mode == Mode::Weather)  s = Screen::Weather;
    else if (_traffic.empty())
    {
        // Nothing tracked: a dim clock overnight (for 15 s a minute the
        // day's summary instead, once there is a day to sum up), otherwise
        // the scanning screen.
        if (isNightActive())
        {
            const bool summary = (now / 1000) % 60 >= 45 &&
                                 (_stats.arrivals() + _stats.departures() >= 10 || _stats.hasYesterday());
            if (summary) InfoScreens::renderDaySummary(g_workFrame, _stats, ((now / 1000) % 60 - 45) * 1000 + now % 1000);
            else         InfoScreens::renderNightScene(g_workFrame, now, _weather, _sky);
        }
        else                 BootSplash::render(g_workFrame, now, _runwayInUse);
        return;
    }

    switch (s)
    {
        case Screen::Arrivals:
        {
            // Departures as a second page (or the only one when nothing is inbound).
            const uint32_t shown = shownFor(s, now);
            const uint32_t half = kScreenDwellMs - 1000;
            const bool dep = departuresPage() && (_arrivalCount == 0 || shown >= half);
            const uint32_t depStart = _arrivalCount ? half : 0;
            if (dep && shown >= depStart + half && InfoScreens::departureHeadings(_departures, _departureCount) >= 2)
            {
                InfoScreens::renderHeadings(g_workFrame, _departures, _departureCount, shown - depStart - half);
                return;
            }
            if (dep)
            {
                char rw[12];
                strlcpy(rw, _runwayDep, sizeof(rw));
                if (char *sp = strchr(rw, ' ')) *sp = '\0';   // the busiest one
                InfoScreens::renderDepartures(g_workFrame, _departures, _departureCount, rw,
                                              _arrivalCount ? shown - half : shown);
            }
            else
                InfoScreens::renderArrivals(g_workFrame, _arrivals, _arrivalCount, _runwayInUse, _weather, now, shown);
            return;
        }
        case Screen::Stats:
        {
            // Today, then the top airlines and types once there are enough of them.
            const uint32_t shown = shownFor(s, now);
            const int pages = statsPages();
            int page = 0;
            uint32_t t = shown;
            if (t >= kScreenDwellMs && pages > 1)
            {
                page = 1 + min((int)((t - kScreenDwellMs) / kStatsPageMs), pages - 2);
                t = (t - kScreenDwellMs) - (page - 1) * kStatsPageMs;
            }
            uint8_t list[10];
            statsPageList(list);
            switch (list[page])
            {
                case 0: InfoScreens::renderStats(g_workFrame, _stats, now, shown); break;
                case 1: InfoScreens::renderRhythm(g_workFrame, _stats, _runwayArr, t); break;
                case 2: InfoScreens::renderTopAirlines(g_workFrame, _stats, t); break;
                case 3: InfoScreens::renderTopTypes(g_workFrame, _stats, t); break;
                case 4: InfoScreens::renderSpots(g_workFrame, _stats, t); break;
                default: InfoScreens::renderYear(g_workFrame, _year, list[page] - 5, t); break;   // 5-9: the year (9.0)
            }
            return;
        }
        case Screen::Globe:
        {
            refreshGlobe(now);
            if (!_globe.valid && _hasCurrent) noteGlobe(_current);
            if (_globe.valid)
                Globe::render(g_workFrame, _globe, shownFor(s, now), time(nullptr));
            else
            {
                // A preview before any flight with a route: New York to London, nearly in.
                Globe::Route demo;
                demo.valid = true;
                strlcpy(demo.ident, "GS101", sizeof(demo.ident));
                strlcpy(demo.from, "New York", sizeof(demo.from));
                strlcpy(demo.to, "London", sizeof(demo.to));
                demo.oLat = 40.64f; demo.oLon = -73.78f; demo.dLat = 51.47f; demo.dLon = -0.45f;
                demo.lat = 51.30f; demo.lon = -1.20f; demo.speedKts = 250;
                Globe::render(g_workFrame, demo, shownFor(s, now), time(nullptr));
            }
            return;
        }
        case Screen::Weather:
            InfoScreens::renderWeather(g_workFrame, _metar, _runwayInUse, now,
                                       s_skyPreviewUntil ? Sky::preview(s_skyPreview) : _sky, _mood, _outlook);
            return;
        case Screen::Lineup:
        {
            LineupScreen::Plane p[LineupScreen::kMax];
            const int n = LineupScreen::collect(_traffic, now, p);
            LineupScreen::render(g_workFrame, p, n, shownFor(s, now), _sky);
            return;
        }
        case Screen::Holding:
        {
            HoldTracker::Row rows[Airport::kMaxHolds + 1];
            int n = _holds.rows(rows, Airport::kMaxHolds + 1, now);
            if (s_screenPreview == 5 && _holds.total(now) == 0)
            {
                // Preview with nothing holding: a busy morning at Heathrow.
                static const HoldTracker::Row kDemo[] = {
                    {"BNN", 4, 9}, {"LAM", 2, 5}, {"BIG", 0, 0}, {"OCK", 3, 7},
                };
                n = 4;
                for (int i = 0; i < n; ++i) rows[i] = kDemo[i];
            }
            InfoScreens::renderHolding(g_workFrame, rows, n, now);
            return;
        }
        default:
            break;
    }
    const bool homeSet = g_config.home_lat != 0 || g_config.home_lon != 0;
    MapRenderer::render(g_workFrame, _traffic, now,
                        _showcaseActive ? NAN : homeSet ? g_config.home_lat : g_config.center_lat,
                        _showcaseActive ? NAN : homeSet ? g_config.home_lon : g_config.center_lon, _runwayArr, _runwayDep,
                        Sky::mapLight(s_skyPreviewUntil ? Sky::preview(s_skyPreview) : _sky));
}

int NeoMatrixDisplay::statsPages() const
{
    uint8_t list[10];
    return statsPageList(list);
}

// The stats pages in order, each only when it has something to show:
// 0 today, 1 runway rhythm, 2 top airlines, 3 types, 4 spotter log.
int NeoMatrixDisplay::statsPageList(uint8_t *out) const
{
    int n = 0;
    out[n++] = 0;
    if (_stats.recentGapSec() || _stats.tightestGapSec()) out[n++] = 1;
    if (_stats.airlineCount() >= 3) out[n++] = 2;
    if (_stats.typeCount() >= 3) out[n++] = 3;
    if (_stats.spotCount()) out[n++] = 4;
    if (yearSeason())
        for (uint8_t p = 5; p <= 9; ++p) out[n++] = p;
    return n;
}

// 26 December to 2 January, with a year logged (9.0).
bool NeoMatrixDisplay::yearSeason() const
{
    if (!_year.days) return false;
    const time_t t = time(nullptr);
    if (t < 1600000000) return false;
    struct tm lt;
    localtime_r(&t, &lt);
    return (lt.tm_mon == 11 && lt.tm_mday >= 26) || (lt.tm_mon == 0 && lt.tm_mday <= 2);
}

// Night watch is on: the setting, the night hours, the board in its normal
// mode, and no showcase, preview or demo the web page has just asked for.
bool NeoMatrixDisplay::nightWatchNow(unsigned long now) const
{
    if (s_screenPreview == 9) return true;   // the web page's "Show night watch"
    if (!g_config.night_watch || !isNightActive() || _mode != Mode::Auto || _showcaseActive) return false;
    if (s_screenPreview && s_screenPreview != 9) return false;
    if (s_mapPreviewUntil || s_skyPreviewUntil) return false;
    if (s_demoUntil && (long)(now - s_demoUntil) < 0) return false;
    return true;
}

void NeoMatrixDisplay::renderNightWatch(unsigned long now)
{
    // Tonight's movements: counted from when night watch began, across the
    // midnight reset of the day's tally.
    const int total = _stats.arrivals() + _stats.departures();
    if (_nwBase < 0) { _nwBase = total; _nwCarry = 0; }
    else if (total < _nwLastTotal) { _nwCarry += _nwLastTotal - _nwBase; _nwBase = 0; }
    _nwLastTotal = total;
    NightWatch::Tracker t;
    t.tonight = _nwCarry + total - _nwBase;
    t.last = _stats.lastMovement();
    t.today = total;
    const time_t wall = time(nullptr);
    if (_arrivalCount > 0 && !isnan(_arrivals[0].etaSec) && wall > 1600000000)
        t.next = wall + (time_t)max(0.0f, _arrivals[0].etaSec - (now - _arrivals[0].dataMs) / 1000.0f);
    const bool homeSet = g_config.home_lat != 0 || g_config.home_lon != 0;
    NightWatch::render(g_workFrame, now, _sky, _weather, _traffic,
                       homeSet ? g_config.home_lat : g_config.center_lat,
                       homeSet ? g_config.home_lon : g_config.center_lon,
                       FlyAcross::facingDegrees(g_config.screen_facing), t);
}
