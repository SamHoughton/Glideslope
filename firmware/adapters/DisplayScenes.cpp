/*
Purpose: The card and its scenes: the next card coming on, the approach card
with its smoothed altitude, the landing / go-around / take-off scenes, and
the route globe for the card's flight. Part of NeoMatrixDisplay.
*/
#include "adapters/DisplayInternal.h"

bool NeoMatrixDisplay::isFreshDeparture(const FlightInfo &f) const
{
    if (isnan(f.baro_altitude) || f.baro_altitude > kFreshDepartureFt) return false;
    return ApproachModel::evaluate(f).phase == ApproachStatus::Departed;
}

// Keep the route of a card's flight for the globe, when both airports are in
// the offline table and they are far enough apart to draw.
void NeoMatrixDisplay::noteGlobe(const Entry &e)
{
    const FlightInfo &f = e.flight;
    Airports::Place o, d;
    if (!Airports::find(f.origin, o) || !Airports::find(f.destination, d) || !strcmp(o.icao, d.icao)) return;
    if (fabsf(o.lat - d.lat) + fabsf(o.lon - d.lon) < 0.5f) return;
    Globe::Route r;
    r.valid = true;
    strlcpy(r.ident, (f.ident_iata.length() ? f.ident_iata : f.ident).c_str(), sizeof(r.ident));
    r.accent = e.accent;
    if (!Airports::shortCity(o.iata, 10, r.from, sizeof(r.from))) strlcpy(r.from, o.iata, sizeof(r.from));
    if (!Airports::shortCity(d.iata, 10, r.to, sizeof(r.to))) strlcpy(r.to, d.iata, sizeof(r.to));
    r.oLat = o.lat; r.oLon = o.lon; r.dLat = d.lat; r.dLon = d.lon;
    _globe = r;
    refreshGlobe(millis());
}

// The aircraft's latest position and state, while it is still the current card.
void NeoMatrixDisplay::refreshGlobe(unsigned long now)
{
    if (!_globe.valid || !_hasCurrent) return;
    const FlightInfo &f = _current.flight;
    if (strcmp(_globe.ident, (f.ident_iata.length() ? f.ident_iata : f.ident).c_str()) != 0) return;
    _globe.lat = f.lat;
    _globe.lon = f.lon;
    _globe.speedKts = f.velocity;
    _globe.landed = _current.landed;
    _globe.landedAt = _current.landedAt;
    const float eta = entryEta(_current, now);
    const time_t t = time(nullptr);
    _globe.eta = (!isnan(eta) && t > 1600000000) ? t + (time_t)eta : 0;
}

void NeoMatrixDisplay::beginNextCard(unsigned long now)
{
    g_oldFrame.copyFrom(g_shownFrame);   // exactly what is on the panel now (card or map)
    const bool fromMap = _ambientActive && _screen == Screen::Map && _mode == Mode::Auto;
    _ambientActive = false;
    _current    = _queue.front();
    _queue.pop_front();
    loadLogo(_current);
    noteGlobe(_current);
    _hasCurrent = true;
    _inTransition = true;
    _transStartMs = now;
    _path = FlyAcross::pathFor(_current.flight.heading, _current.flight.vertical_rate,
                               g_config.screen_facing);
    // Coming from the map with the aircraft on it: zoom in on its icon.
    _zoomFromMap = false;
    if (fromMap && !isnan(_current.flight.lat))
    {
        MapRenderer::project(_current.flight.lat, _current.flight.lon, _zoomX, _zoomY);
        _zoomFromMap = _zoomX >= 4 && _zoomY >= 4 && _zoomX < FrameCanvas::W - 4 && _zoomY < FrameCanvas::H - 4;
    }
    const RareSpotter::Reason why = RareSpotter::check(_current.flight);
    if (why != RareSpotter::None)
    {
        RareSpotter::banner(why, _current.flight, _flourishLine1, sizeof(_flourishLine1),
                            _flourishLine2, sizeof(_flourishLine2));
        _flourishActive  = true;
        _flourishStartMs = now;
        Log.printf("Display: rare spot %s: %s %s\n", _current.flight.ident.c_str(), _flourishLine1, _flourishLine2);
        const FlightInfo &f = _current.flight;
        Notify::post(Notify::Rare, 3, "star", _flourishLine1, "%s %s %s, %s to %s",
                     (f.ident_iata.length() ? f.ident_iata : f.ident).c_str(), _flourishLine2,
                     f.registration.c_str(),
                     (f.origin.code_iata.length() ? f.origin.code_iata : f.origin.code_icao).c_str(),
                     (f.destination.code_iata.length() ? f.destination.code_iata : f.destination.code_icao).c_str());
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
        Notify::post(Notify::GoAround, 4, "arrows_counterclockwise", "Go-around",
                     "%s went around at %.0f ft%s%s", (f.ident_iata.length() ? f.ident_iata : f.ident).c_str(),
                     f.baro_altitude, e.runway[0] ? " off " : "", e.runway);
        return;
    }
    if (onFinal && descending) e.finalMs = now;
}

void NeoMatrixDisplay::startScene(unsigned long now, LandingScene::Kind kind, bool demo)
{
    _sceneKind      = kind;
    _sceneSky       = (s_skyPreviewUntil && (demo || _showcaseActive)) ? Sky::preview(s_skyPreview) : _sky;
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
    const char *destCity = g_config.route_cities ? Airports::city(f.destination) : nullptr;
    if (_sceneKind == LandingScene::Takeoff && dest.length() && !AirportPack::isHome(f.destination))
    {
        snprintf(caption, sizeof(caption), "%s TO %s", id.c_str(), destCity ? destCity : dest.c_str());
        if (FrameCanvas::textWidth(caption) > FrameCanvas::W - 4)   // too long for the line: the code
            snprintf(caption, sizeof(caption), "%s TO %s", id.c_str(), dest.c_str());
    }
    else
        snprintf(caption, sizeof(caption), "%s", id.c_str());
    const AircraftSprites::Livery livery = AircraftSprites::liveryFor(f, _current.accent);
    LandingScene::render(g_workFrame, t, AircraftSprites::getScene(kind), _current.accent, !known,
                         rightward, caption, _current.runway, _sceneKind, _sceneSky, &livery);
    present();
    return true;
}
