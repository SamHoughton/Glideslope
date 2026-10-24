/*
Purpose: Drive the HUB75 panel via ESP32-HUB75-MatrixPanel-I2S-DMA.
Responsibilities:
- Initialise the matrix (HD-WF2 pin map when built with HD_WF2).
- Queue new contacts and fly each one in over the previous card.
- Render frames into an off-screen FrameCanvas on a dedicated task and push
  only the pixels that changed to the panel.
*/
#include "adapters/DisplayInternal.h"
#include "utils/Notify.h"

// The panel mirror (declared in NeoMatrixDisplay.h; the web page reads it).
FrameCanvas        g_shownFrame;

uint32_t displayFramesDrawn() { return s_framesDrawn; }

void requestSplashPreview(uint32_t durationMs) { s_splashRequestMs = durationMs ? durationMs : 1; }

void requestMapPreview(uint32_t durationMs) { s_mapRequestMs = durationMs ? durationMs : 1; }

// Ambient screen previews from the web page: 1 stats, 2 clock.

void noteWebDemo() { s_demoUntil = millis() + 60000; }

void requestScreenPreview(uint8_t which, uint32_t durationMs)
{
    s_screenPreviewUntil = millis() + durationMs;
    s_screenPreview = which;
}

void requestSpriteGallery(uint32_t durationMs) { s_galleryRequestMs = durationMs ? durationMs : 1; }

// Replay request from the web task: 0 none, 1 real path, 2 force L→R, 3 force R→L.

void requestTakeoffDemo() { s_takeoffDemo = true; }

void requestAlertDemo() { s_alertDemo = true; }
void requestOverheadDemo() { s_spotDemo = true; }
const FrameCanvas &shownFrame() { return g_shownFrame; }

void requestSkyPreview(const char *look, bool landing)
{
    strlcpy(s_skyPreview, look, sizeof(s_skyPreview));
    s_skyPreviewUntil = millis() + (landing ? 45000 : 12000);
    if (landing) s_skyDemo = true;   // a landing in that sky, straight away
}
void requestRunwayChangeDemo() { s_runwayDemo = true; }

void requestShowcase(const char *look)
{
    if (look && look[0])
    {
        strlcpy(s_skyPreview, look, sizeof(s_skyPreview));
        s_skyPreviewUntil = millis() + 50000;
    }
    s_showcaseRequest = true;
}

uint32_t showcaseFrameMs() { return s_frameShowMs; }

void requestPanelMessage(const char *text, uint32_t ms)
{
    strlcpy(s_panelMsg, text, sizeof(s_panelMsg));
    s_panelMsgMs = ms;
    s_panelMsgPending = true;
}

void requestGoAroundDemo() { s_goAroundDemo = true; }

void requestRareSpotDemo() { s_rareDemo = true; }

void requestMoodDemo(uint8_t style) { s_moodDemo = style; }

void requestLandingReplay() { s_landingReplay = true; }

void requestFlyAcrossReplay(int forceDirection)
{
    s_replayRequest = forceDirection > 0 ? 2 : forceDirection < 0 ? 3 : 1;
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

        const ApproachStatus st = ApproachModel::evaluate(f);
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
            std::vector<uint16_t> logo;   // keep the card's logo across the update
            logo.swap(_current.flight.airline_logo_rgb565);
            _current.flight = f;
            logo.swap(_current.flight.airline_logo_rgb565);
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

}

// The card coming on screen: its logo from flash, and the accent colour from it.
void NeoMatrixDisplay::loadLogo(Entry &e)
{
    if (e.flight.airline_logo_rgb565.empty() && _logos && e.flight.logo_code.length())
        _logos->getAirlineLogo(e.flight.logo_code, e.flight.airline_logo_rgb565);
    e.accent = CardRenderer::accentFor(e.flight);
}

void NeoMatrixDisplay::setArrivals(const InfoScreens::Arrival *rows, int n)
{
    Lock l(_lock);
    _arrivalCount = min(n, InfoScreens::kMaxArrivals);
    for (int i = 0; i < _arrivalCount; ++i) _arrivals[i] = rows[i];
}

void NeoMatrixDisplay::setDepartures(const InfoScreens::Departure *rows, int n)
{
    Lock l(_lock);
    _departureCount = min(n, InfoScreens::kMaxDepartures);
    for (int i = 0; i < _departureCount; ++i) _departures[i] = rows[i];
}

void NeoMatrixDisplay::noteTraffic(const std::vector<StateVector> &states)
{
    const unsigned long now = millis();
    Lock l(_lock);
    _stats.noteTraffic(states);
    checkRecords();
    {
        char id[10], rwy[4];
        int alt = 0;
        if (_stats.takeGoAround(id, sizeof(id), rwy, sizeof(rwy), alt))
        {
            Log.printf("Traffic: %s GO-AROUND%s%s (%d ft)\n", id, rwy[0] ? " off " : "", rwy, alt);
            Notify::post(Notify::GoAround, 4, "arrows_counterclockwise", "Go-around",
                         "%s went around at %d ft%s%s", id, alt, rwy[0] ? " off " : "", rwy);
        }
    }
    _holds.noteStates(states, now);
    if (_runways.update(states, now))
    {
        strlcpy(_rwyFrom, _runways.changeFrom(), sizeof(_rwyFrom));
        strlcpy(_rwyTo, _runways.changeTo(), sizeof(_rwyTo));
        _rwyChangeMs = now;
        _rwyChangeActive = true;
        Notify::post(Notify::Runway, 3, "airplane_arriving", "Runway change",
                     "%s now landing on %s (was %s)", g_airport.name, _rwyTo, _rwyFrom);
    }
    strlcpy(_runwayInUse, _runways.mainArrival(), sizeof(_runwayInUse));
    _runways.arrivals(_runwayArr, sizeof(_runwayArr));
    _runways.departures(_runwayDep, sizeof(_runwayDep));
}

void NeoMatrixDisplay::noteStack(int stack, const std::vector<StateVector> &states)
{
    const unsigned long now = millis();
    Lock l(_lock);
    _holds.noteStack(stack, states, now);
}

int NeoMatrixDisplay::holdingSummary(char *out, size_t len, int *longestMin)
{
    HoldTracker::Row rows[Airport::kMaxHolds + 1];
    int n, total;
    {
        Lock l(_lock);
        n = _holds.rows(rows, Airport::kMaxHolds + 1, millis());
    }
    out[0] = '\0';
    total = 0;
    if (longestMin) *longestMin = 0;
    for (int i = 0; i < n; ++i)
    {
        if (!rows[i].count) continue;
        total += rows[i].count;
        if (longestMin && rows[i].longestMin > *longestMin) *longestMin = rows[i].longestMin;
        char part[32];
        snprintf(part, sizeof(part), "%s%s %u (%u min)", out[0] ? ", " : "", rows[i].name,
                 (unsigned)rows[i].count, (unsigned)rows[i].longestMin);
        strlcat(out, part, len);
    }
    return total;
}

void NeoMatrixDisplay::todayCounts(int &arrivals, int &departures)
{
    Lock l(_lock);
    arrivals = _stats.arrivals();
    departures = _stats.departures();
}

void NeoMatrixDisplay::dailySummary(char *out, size_t len)
{
    Lock l(_lock);
    int busiestHour = 0, busiestN = 0;
    for (int h = 0; h < 24; ++h)
    {
        const int n = _stats.arrivalsInHour(h) + _stats.departuresInHour(h);
        if (n > busiestN) { busiestN = n; busiestHour = h; }
    }
    int an = 0;
    const String airline = _stats.busiestAirline(an);
    const String rare = _stats.rarestType();
    snprintf(out, len, "%d movements (%d in, %d out). Busiest hour %02d:00 (%d).%s%s%s%s%s",
             _stats.arrivals() + _stats.departures(), _stats.arrivals(), _stats.departures(),
             busiestHour, busiestN,
             airline.length() ? " Top airline " : "", airline.c_str(),
             rare.length() ? ". Rarest " : "", rare.c_str(), ".");
    if (_stats.goArounds())
    {
        char ga[24];
        snprintf(ga, sizeof(ga), " %d go-around%s.", _stats.goArounds(), _stats.goArounds() == 1 ? "" : "s");
        strlcat(out, ga, len);
    }
}

void NeoMatrixDisplay::runwaysInUse(char *arr, size_t arrLen, char *dep, size_t depLen)
{
    Lock l(_lock);
    strlcpy(arr, _runwayArr, arrLen);
    strlcpy(dep, _runwayDep, depLen);
}

size_t NeoMatrixDisplay::statsSize()
{
    Lock l(_lock);
    return _stats.dirty() ? _stats.serialisedSize() : 0;
}

bool NeoMatrixDisplay::statsSnapshot(std::vector<uint8_t> &out)
{
    Lock l(_lock);
    if (!_stats.dirty()) return false;
    _stats.serialise(out);
    return true;
}

void NeoMatrixDisplay::loadStats()
{
    Lock l(_lock);
    _stats.load();
}

void NeoMatrixDisplay::setWeather(const Metar &m)
{
    char line[40];
    Weather::friendly(m, line, sizeof(line));
    Lock l(_lock);
    _metar = m;
    strlcpy(_weather, line, sizeof(_weather));
}

void NeoMatrixDisplay::showSpot(const OverheadScene::Info &info)
{
    Lock l(_lock);
    _spot = info;
    _spotPending = true;
    _stats.noteSpot(strncmp(info.label, "WATCH", 5) == 0 || strncmp(info.label, "Watch", 5) == 0
                        ? DailyStats::Watchlist : DailyStats::Overhead,
                    info.ident, info.type);
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
    _stats.noteSpot(DailyStats::Emergency, ident, code);
}

bool NeoMatrixDisplay::takeLogbookDay(Logbook::Day &day, std::vector<Logbook::Tally> &tallies)
{
    Lock l(_lock);
    return _stats.takeDay(day, tallies);
}

int NeoMatrixDisplay::takeLogbookEvents(Logbook::Event *out, int cap)
{
    Lock l(_lock);
    return _stats.takeEvents(out, cap);
}

void NeoMatrixDisplay::setRecords(const Logbook::Records &r)
{
    Lock l(_lock);
    _records = r;
}

void NeoMatrixDisplay::showNews(const char *line1, const char *line2)
{
    if (!line1 || !line1[0]) return;
    Lock l(_lock);
    strlcpy(_news1, line1, sizeof(_news1));
    strlcpy(_news2, line2 ? line2 : "", sizeof(_news2));
    _newsStyle = 0;
    _newsPending = true;
}

void NeoMatrixDisplay::showMood(uint8_t style, const char *line1, const char *line2)
{
    if (!line1 || !line1[0]) return;
    Lock l(_lock);
    strlcpy(_news1, line1, sizeof(_news1));
    strlcpy(_news2, line2 ? line2 : "", sizeof(_news2));
    _newsStyle = style;
    _newsQueuedMs = millis();
    _newsPending = true;
}

void NeoMatrixDisplay::setAirfield(uint8_t mood, const char *outlook)
{
    Lock l(_lock);
    _mood = mood;
    strlcpy(_outlook, outlook ? outlook : "", sizeof(_outlook));
}

// Today beating the all-time bests, as it happens (once each a day): the
// busiest day, the most go-arounds, the tightest gap. Under the lock.
void NeoMatrixDisplay::checkRecords()
{
    const uint32_t today = _stats.date();
    if (!today || !_records.since) return;   // nothing to beat until a day is logged
    if (today != _recordsDate) { _recordsDate = today; _beatDay = _beatGa = _beatGap = false; }
    char l2[24];
    const int moves = _stats.arrivals() + _stats.departures();
    if (!_beatDay && _records.bestDay && moves > _records.bestDay)
    {
        _beatDay = true;
        snprintf(l2, sizeof(l2), "%d today", moves);
        strlcpy(_news1, "Busiest day ever", sizeof(_news1));
        strlcpy(_news2, l2, sizeof(_news2));
        _newsStyle = 0;
        _newsPending = true;
        Log.printf("Logbook: today is the busiest day yet (%d, was %u)\n", moves, _records.bestDay);
    }
    if (!_beatGa && _stats.goArounds() > _records.bestGa && _records.bestGa)
    {
        _beatGa = true;
        snprintf(l2, sizeof(l2), "%d go-arounds", _stats.goArounds());
        strlcpy(_news1, "Most in a day", sizeof(_news1));
        strlcpy(_news2, l2, sizeof(_news2));
        _newsStyle = 0;
        _newsPending = true;
    }
    const int gap = _stats.tightestGapSec();
    if (!_beatGap && gap && _records.tightest && gap < _records.tightest)
    {
        _beatGap = true;
        snprintf(l2, sizeof(l2), "%d:%02d apart", gap / 60, gap % 60);
        strlcpy(_news1, "Tightest gap ever", sizeof(_news1));
        strlcpy(_news2, l2, sizeof(_news2));
        _newsStyle = 0;
        _newsPending = true;
    }
}

void NeoMatrixDisplay::setOriginWeather(const Metar &m)
{
    if (!m.valid || !m.station[0]) return;
    Lock l(_lock);
    int slot = 0;
    for (int i = 0; i < 6; ++i)
    {
        if (strcmp(_origins[i].icao, m.station) == 0) { slot = i; break; }
        if (_origins[i].atMs < _origins[slot].atMs) slot = i;   // else the oldest
    }
    OriginWx &o = _origins[slot];
    strlcpy(o.icao, m.station, sizeof(o.icao));
    o.tempC = (int8_t)(m.tempC == -99 ? -128 : max(-100, min(60, m.tempC)));
    o.cloud = strstr(m.raw, " OVC") || strstr(m.raw, " VV") ? 3 : strstr(m.raw, " BKN") ? 2
            : (strstr(m.raw, " SCT") || strstr(m.raw, " FEW")) ? 1 : 0;
    strlcpy(o.wx, m.wx, sizeof(o.wx));
    o.visM = (int16_t)min(9999, m.visM);
    o.atMs = millis() | 1;
}

bool NeoMatrixDisplay::hasOriginWeather(const char *icao)
{
    Lock l(_lock);
    Metar m;
    return originMetar(icao, m);
}

bool NeoMatrixDisplay::originMetar(const char *icao, Metar &out) const
{
    for (const OriginWx &o : _origins)
        if (o.atMs && strcmp(o.icao, icao) == 0 && millis() - o.atMs < 3UL * 3600 * 1000)
        {
            out = Metar();
            out.valid = true;
            strlcpy(out.station, o.icao, sizeof(out.station));
            out.tempC = o.tempC == -128 ? -99 : o.tempC;
            strlcpy(out.wx, o.wx, sizeof(out.wx));
            out.visM = o.visM;
            out.cavok = o.cloud == 0 && !o.wx[0];
            static const char *const kCloud[] = {"", " SCT", " BKN", " OVC"};   // what the sky reads
            strlcpy(out.raw, kCloud[o.cloud & 3], sizeof(out.raw));
            return true;
        }
    return false;
}

void NeoMatrixDisplay::todayJson(String &out)
{
    // Copied under the lock, written out after it: a web client can be slow.
    DailyStats::Spot spots[DailyStats::kMaxSpots];
    DailyStats::RunwayCount rw[4];
    int nSpots, nRw, arr, dep, recent, avg, tight, tightMin, peakH, peakN, spotsToday, goArounds;
    char tightId[8];
    {
        Lock l(_lock);
        nSpots = _stats.spotCount();
        for (int i = 0; i < nSpots; ++i) spots[i] = _stats.spot(i);
        nRw = _stats.runways(rw, 4);
        arr = _stats.arrivals(); dep = _stats.departures();
        recent = _stats.recentGapSec(); avg = _stats.averageGapSec();
        tight = _stats.tightestGapSec(); tightMin = _stats.tightestAtMinute();
        strlcpy(tightId, _stats.tightestIdent(), sizeof(tightId));
        peakH = _stats.busiestHour(peakN);
        spotsToday = _stats.spotsToday();
        goArounds = _stats.goArounds();
    }
    // Idents and types are letters, digits and dashes; anything else is dropped.
    auto clean = [](const char *s, char *o, size_t len) {
        size_t n = 0;
        for (; *s && n + 1 < len; ++s)
            if (isalnum((unsigned char)*s) || *s == '-' || *s == ' ') o[n++] = *s;
        o[n] = '\0';
    };
    static const char *const kKinds[] = {"rare", "first", "watch", "overhead", "goaround", "emergency"};
    out = "";
    out.reserve(160 + nSpots * 64 + nRw * 24);
    char buf[96], a[12], b[12];
    snprintf(buf, sizeof(buf), "{\"arrivals\":%d,\"departures\":%d,\"every\":%d,\"average\":%d,\"tightest\":%d,",
             arr, dep, recent, avg, tight);
    out += buf;
    clean(tightId, a, sizeof(a));
    snprintf(buf, sizeof(buf), "\"tightest_ident\":\"%s\",\"tightest_at\":%d,\"busiest_hour\":%d,\"busiest_n\":%d,",
             a, tightMin, peakH, peakN);
    out += buf;
    snprintf(buf, sizeof(buf), "\"spotted\":%d,\"go_arounds\":%d,\"runways\":[", spotsToday, goArounds);
    out += buf;
    for (int i = 0; i < nRw; ++i)
    {
        clean(rw[i].name, a, sizeof(a));
        snprintf(buf, sizeof(buf), "%s{\"name\":\"%s\",\"n\":%u}", i ? "," : "", a, (unsigned)rw[i].n);
        out += buf;
    }
    out += "],\"spots\":[";
    for (int i = 0; i < nSpots; ++i)
    {
        clean(spots[i].ident, a, sizeof(a));
        clean(spots[i].what, b, sizeof(b));
        snprintf(buf, sizeof(buf), "%s{\"at\":%u,\"kind\":\"%s\",\"ident\":\"%s\",\"what\":\"%s\"}", i ? "," : "",
                 (unsigned)spots[i].minute, spots[i].kind < 6 ? kKinds[spots[i].kind] : "other", a, b);
        out += buf;
    }
    out += "]}";
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

// Blend the work frame with `from`: k = 0 shows `from`, 1 shows the work frame.
void NeoMatrixDisplay::crossFade(const PackedFrame &from, float k)
{
    uint16_t *px = g_workFrame.pixels();
    for (int i = 0; i < PackedFrame::N; ++i)
    {
        const uint16_t o = from.raw(i);
        if (PackedFrame::to444(px[i]) == o) continue;   // unchanged: keep the full-depth pixel
        const Rgb a = FrameCanvas::unpack(PackedFrame::to565(o)), b = FrameCanvas::unpack(px[i]);
        px[i] = FrameCanvas::pack(Rgb{ (uint8_t)(a.r + (b.r - a.r) * k), (uint8_t)(a.g + (b.g - a.g) * k),
                                       (uint8_t)(a.b + (b.b - a.b) * k) });
    }
}

void NeoMatrixDisplay::updateTraffic(const std::vector<TrafficPoint> &points)
{
    Lock l(_lock);
    _traffic.update(points, millis());
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
    s_frameShowMs = _showcaseActive ? (uint32_t)(millis() - _showcaseStartMs) : UINT32_MAX;
}
