#pragma once

#include <stdint.h>
#include <vector>
#include <deque>
#include <map>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "interfaces/BaseDisplay.h"
#include "display/FrameCanvas.h"
#include "display/FlyAcross.h"
#include "display/Globe.h"
#include "display/OverheadScene.h"
#include "display/ApproachModel.h"
#include "display/Traffic.h"
#include "display/DailyStats.h"
#include "display/LogbookTypes.h"
#include "display/RunwayTracker.h"
#include "display/Sky.h"
#include "display/InfoScreens.h"
#include "display/LandingScene.h"
#include "utils/Weather.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// Mirror of what is currently on the panel; the web preview reads this.
extern FrameCanvas g_shownFrame;

// Frames drawn by the display task since boot (health reporting).
uint32_t displayFramesDrawn();

// Ask the display to show the aircraft sprite test grid for durationMs.
// Safe to call from the web task.
void requestSpriteGallery(uint32_t durationMs);

// Show the boot / scanning screen for durationMs (preview). Safe from the web task.
void requestSplashPreview(uint32_t durationMs);

// Show the London map for durationMs (preview). Safe from the web task.
void requestMapPreview(uint32_t durationMs);

// Preview a screen (1 = stats, 2 = clock, 3 = arrivals, 4 = weather) for durationMs. Safe from the web task.
void requestScreenPreview(uint8_t which, uint32_t durationMs);
// A demo from the web page: night watch stands aside for a minute so it shows.
void noteWebDemo();

// Replay the fly-across onto the current card. forceDirection: 0 = the
// flight's real direction, +1 = left-to-right, -1 = right-to-left (testing).
void requestFlyAcrossReplay(int forceDirection = 0);

// Replay the landing animation for the current card (testing / demo).
void requestLandingReplay();

// Play the go-around animation on the current card (demo).
void requestGoAroundDemo();
void requestGoAroundAlertDemo();   // 8.3: the alert for one in range

// Show the rare-spot flourish on the current card (demo).
void requestRareSpotDemo();
// The airfield's mood banners (8.2): 1 low visibility, 2 thunderstorm, 3 runway change due;
// 4 a special livery (9.0).
void requestMoodDemo(uint8_t style);

// Play the take-off scene for the current card (demo).
void requestTakeoffDemo();

// Show a sample emergency-squawk alert (demo).
void requestAlertDemo();
void requestOverheadDemo();
// The frame on the panel now (for the alert pictures).
const FrameCanvas &shownFrame();
void requestRunwayChangeDemo();
// Previews a sky ("night+rain", see Sky::preview): replays the card's landing
// in it (or the showcase when there is no card). Real flights keep the real sky.
void requestSkyPreview(const char *look, bool landing = true);

// Scripted ~35 s showcase (for demos and the README recording): a fictional
// flight, GS101, flies in on final for 27L with the Glideslope badge, lands,
// then the map, arrivals board and weather, and a take-off. Safe from the web task.
// The showcase (for the README GIF too): the board stops fetching while it
// runs, the map hides the home marker, and look ("golden", see Sky::preview)
// sets the sky, "" for the real one.
void requestShowcase(const char *look = "");
// Showcase time (ms) of the frame on the panel; UINT32_MAX outside the showcase.
uint32_t showcaseFrameMs();

// Short status text on the panel for a few seconds (e.g. update progress). Safe from the web task.
void requestPanelMessage(const char *text, uint32_t ms = 4000);

/*
Card flow: each fetch's flight list goes through displayFlights(). A flight
not seen in the last kSeenCooldownMs is queued as a new contact. A card stays
up for at least display_cycle_seconds; then the next queued flight flies in.
With nothing queued the current card stays up, with live telemetry while the
flight remains in the list.

Threading: after startTask() all panel drawing happens on the display task.
Public methods only touch shared state under _lock.
*/
class NeoMatrixDisplay : public BaseDisplay
{
public:
    NeoMatrixDisplay();
    ~NeoMatrixDisplay() override;

    bool initialize() override;
    void clear() override;
    // Hand over the latest fetch results (call once per fetch).
    void displayFlights(const std::vector<FlightInfo> &flights) override;
    // Every tracked aircraft, for the map (call once per fetch).
    void updateTraffic(const std::vector<TrafficPoint> &points);
    // Short status message (shown for a few seconds once the task is running).
    void displayMessage(const String &message);
    void showLoading();
    // Start the display task; from then on it owns the panel.
    void startTask();
    TaskHandle_t task() const { return _task; }

    // Frames drawn by the display task so far (heartbeat diagnostics).
    uint32_t framesDrawn() const { return _frames; }

    bool showcaseRunning() const { return _showcaseActive; }

    // Copy of the flight on screen, without its logo; false when none (scanning screen).
    bool currentFlight(FlightInfo &out);
    // Today for the web page (7.1): movements, the runway rhythm and the
    // spotter log, as JSON.
    void todayJson(String &out);
    // The weather where an arrival came from (8.0), from the weather fetch.
    void setOriginWeather(const Metar &m);
    // The logbook (8.1): the finished day and the sightings to keep, taken
    // from the tally under the lock; the records to beat; news for the
    // panel (a record or milestone), shown when nothing else has it.
    bool takeLogbookDay(Logbook::Day &day, std::vector<Logbook::Tally> &tallies);
    int  takeLogbookEvents(Logbook::Event *out, int cap);
    void setRecords(const Logbook::Records &r);
    void showNews(const char *line1, const char *line2);
    // The airfield's mood and the runway outlook (8.2): a banner (style 1 low
    // visibility, 2 thunderstorm, 3 runway change due), and what the weather
    // screen says (mood: Weather::Mood; outlook: "To 09 at 19:00" or "").
    void showMood(uint8_t style, const char *line1, const char *line2);
    void setAirfield(uint8_t mood, const char *outlook);
    // The year so far (9.0), from the logbook: on the stats screen from 26
    // December to 2 January (and on demand).
    void setYear(const Logbook::Year &y);
    void noteCrosswindLanding();   // at touchdown, under the lock (9.1)
    bool takeNewAirline(char *code, size_t len);   // see DailyStats (9.1)
    // Every aircraft in range for the web page's live map (9.1); with
    // withAirport, the runways and holding stacks too.
    void trafficJson(String &out, bool withAirport);
    bool hasOriginWeather(const char *icao);

    // Today's stats from every aircraft in range (call once per fetch), and
    // their persistence: snapshot under the lock, write the file outside it.
    void noteTraffic(const std::vector<StateVector> &states);
    // Runways in use from that traffic ("27L", "26R 26L"; "" if none).
    void runwaysInUse(char *arr, size_t arrLen, char *dep, size_t depLen);
    // The aircraft around named holding stack i (Airport::holds), once a minute.
    void noteStack(int stack, const std::vector<StateVector> &states);
    // Holding now: "BNN 4 (9 min), OCK 2 (3 min)" ("" if none); returns the total.
    int holdingSummary(char *out, size_t len, int *longestMin = nullptr);
    // Today's arrivals and departures so far.
    void todayCounts(int &arrivals, int &departures);
    // Today in a sentence or two, for the evening round-up.
    void dailySummary(char *out, size_t len);
    bool statsSnapshot(std::vector<uint8_t> &out);   // false if unchanged
    size_t statsSize();                              // bytes a snapshot needs; 0 if unchanged
    void loadStats();

    // Arrivals board rows, soonest first (call once per fetch).
    void setArrivals(const InfoScreens::Arrival *rows, int n);
    // Recent departures, newest first: a second page of the board when
    // switched on (g_config.screens bit 32).
    void setDepartures(const InfoScreens::Departure *rows, int n);

    // Heathrow weather for the weather screen, arrivals board and night clock.
    void setWeather(const Metar &m);

    // Emergency squawk: takes over the panel for a few seconds. Safe from any task.
    void raiseAlert(const char *code, const char *meaning, const char *ident, const char *detail);
    // An aircraft overhead or on the watchlist: its pass across the sky.
    void showSpot(const OverheadScene::Info &info);

    // Where card logos come from (loaded only for the card on screen).
    void setLogoStore(class BaseLogoStore *store) { _logos = store; }

private:
    struct Entry
    {
        FlightInfo    flight;
        unsigned long dataMs = 0;          // when this telemetry arrived
        Rgb           accent{90, 170, 255};
        char          runway[4] = "";      // last runway seen for this flight
        bool          landingPlayed = false;
        bool          landed = false;      // card shows LANDED after the animation
        time_t        landedAt = 0;        // touchdown (Unix time), 0 if clock unsynced
        float         shownAltFt = NAN;    // smoothed altitude on the card
        unsigned long finalMs = 0;         // last report that had it on final
        bool          goAround = false;    // card shows GO AROUND
        unsigned long goAroundMs = 0;
        bool          goAroundAnimPending = false;
        bool          takeoffPlayed = false;   // departure scene shown
        bool          breakTaken = false;      // the break after its landing has started
        unsigned long altMs = 0;           // when shownAltFt was last updated
    };

    class BaseLogoStore *_logos = nullptr;
    void                 loadLogo(Entry &e);
    MatrixPanel_I2S_DMA *_matrix = nullptr;
    SemaphoreHandle_t    _lock   = nullptr;
    TaskHandle_t         _task   = nullptr;
    volatile uint32_t    _frames = 0;

    // Card state (guarded by _lock)
    bool                 _hasCurrent = false;
    Entry                _current;
    unsigned long        _shownSinceMs = 0;
    std::deque<Entry>    _queue;
    std::map<String, unsigned long> _seenMs;   // ident -> last time in a fetch
    RunwayTracker        _runways;               // from every aircraft in range
    HoldTracker          _holds;                 // aircraft in holding patterns
    char                 _runwayInUse[4] = "";  // main arrival runway, e.g. "27L"
    char                 _runwayArr[12] = "", _runwayDep[12] = "";   // all in use
    bool                 _rwyChangeActive = false;   // RUNWAY CHANGE on the panel
    unsigned long        _rwyChangeMs = 0;
    char                 _rwyFrom[4] = "", _rwyTo[4] = "";
    Entry                _nextApproach;          // closest-to-touchdown aircraft not on the card
    bool                 _hasNextApproach = false;

    // Transition state (display task only)
    bool                 _inTransition = false;
    // From the map, the card comes in by zooming on the aircraft's icon
    // instead of the fly-across (map pixel the zoom closes in on).
    bool                 _zoomFromMap = false;
    float                _zoomX = 0, _zoomY = 0;
    unsigned long        _transStartMs = 0;
    FlyAcross::Path      _path;            // from the incoming flight's track + vertical rate
    // Ambient screen (map / scanning) state (display task only)
    bool                 _ambientActive  = false;
    unsigned long        _ambientSinceMs = 0;
    unsigned long        _ambientUntilMs = 0;
    TrafficTracker       _traffic;               // guarded by _lock

    // Button-selected mode (display task only)
    enum class Mode : uint8_t { Auto, Map, Arrivals, Stats, Weather };
    // Rotation screens; bit n of g_config.screens enables Screen n. Bit 5
    // is the departures page of the arrivals board, not a screen of its own.
    // The index is the bit in g_config.screens (FinalPages is bit 7, a setting, not a screen).
    enum class Screen : uint8_t { Map, Arrivals, Stats, Weather, Holding, Departures, Globe, FinalPages, Lineup, Count };
    // Overhead / watchlist scene (display task; _spotPending set under the lock).
    OverheadScene::Info  _spot;
    bool                 _spotPending = false, _spotActive = false;
    unsigned long        _spotStartMs = 0;
    // The route globe: the latest card's flight with both airports known.
    Globe::Route         _globe;
    void                 noteGlobe(const Entry &e);
    void                 refreshGlobe(unsigned long now);
    int                  statsPages() const;
    // Logbook news and records.
    Logbook::Records     _records;
    uint32_t             _recordsDate = 0;            // the day the "today beat it" flags belong to
    bool                 _beatDay = false, _beatGa = false, _beatGap = false;
    char                 _news1[20] = "", _news2[24] = "";
    bool                 _newsPending = false, _newsActive = false;
    unsigned long        _newsMs = 0;
    uint8_t              _newsStyle = 0;              // 0 gold (logbook), else a mood banner
    unsigned long        _newsQueuedMs = 0;
    uint8_t              _mood = 0;
    Logbook::Year        _year;
    bool                 yearSeason() const;
    // A go-around anywhere in range (8.3): taken over the panel once.
    char                 _gaIdent[10] = "", _gaRwy[4] = "";
    int                  _gaAlt = 0;
    bool                 _gaPending = false, _gaActive = false;
    unsigned long        _gaQueuedMs = 0, _gaMs = 0;
    char                 _outlook[20] = "";
    void                 checkRecords();
    // Night watch (8.1).
    bool                 nightWatchNow(unsigned long now) const;
    void                 renderNightWatch(unsigned long now);
    int                  _nwBase = -1, _nwCarry = 0, _nwLastTotal = 0;   // tonight's count across midnight
    // Origins' weather, compact: the last few airports asked about.
    struct OriginWx { char icao[5]; int8_t tempC; uint8_t cloud; char wx[8]; int16_t visM; uint32_t atMs; };
    OriginWx             _origins[6] = {};
    bool                 originMetar(const char *icao, Metar &out) const;   // under the lock
    int                  statsPageList(uint8_t *out) const;   // out: room for 10
    // On final: the card alternates with the route globe and the aircraft
    // page, back to the card for good 40 s out (renderLanding comes first
    // every frame, so a landing always plays).
    uint8_t              _storyPage = 0;     // 0 card, 1 globe, 2 aircraft
    unsigned long        _storyFadeMs = 0;
    uint8_t              storyPage(unsigned long now, uint32_t &pageMs);
    // renderFrame's stages (adapters/DisplayRhythm.cpp).
    bool                 renderOverlays(unsigned long now, uint32_t &wait);
    bool                 renderTransition(unsigned long now, uint32_t &wait);
    uint32_t             renderRhythm(unsigned long now);
    void                 renderHeld(unsigned long now);
    Screen               _screen = Screen::Weather;   // so the first rotation starts with the map
    Screen               _shownScreen = Screen::Count;
    unsigned long        _shownScreenMs = 0, _shownScreenLastMs = 0;
    uint32_t             shownFor(Screen s, unsigned long now);
    unsigned long        _screenSinceMs = 0;
    unsigned long        _interludeUntilMs = 0;      // break after a landing ends
    Mode                 _mode = Mode::Auto;
    bool                 _buttonDown = false;
    unsigned long        _buttonChangeMs = 0;
    char                 _caption[12] = "";
    unsigned long        _captionUntilMs = 0;

    // Rare-spot flourish before a fly-across (display task only)
    bool                 _flourishActive = false;
    unsigned long        _flourishStartMs = 0;
    char                 _flourishLine1[20] = "", _flourishLine2[20] = "";

    DailyStats           _stats;                  // guarded by _lock
    InfoScreens::Arrival _arrivals[InfoScreens::kMaxArrivals];   // guarded by _lock
    int                  _arrivalCount = 0;
    InfoScreens::Departure _departures[InfoScreens::kMaxDepartures];   // guarded by _lock
    int                  _departureCount = 0;
    bool                 departuresPage() const;
    char                 _weather[40] = "";       // guarded by _lock
    Metar                _metar;                  // guarded by _lock
    Sky::Look            _sky;                    // the light now (refreshed every 30 s)
    unsigned long        _skyMs = 0;
    Sky::Look            _sceneSky;               // the light the current scene is drawn in

    // Emergency-squawk alert (guarded by _lock)
    char                 _alertCode[6] = "", _alertMeaning[12] = "", _alertIdent[12] = "", _alertDetail[24] = "";
    unsigned long        _alertStartMs = 0;
    bool                 _alertActive = false;

    bool                 _landingActive  = false;
    bool                 _landingDemo    = false;
    LandingScene::Kind   _sceneKind = LandingScene::Landing;
    unsigned long        _landingStartMs = 0;

    // Message override (guarded by _lock)
    String               _message;
    unsigned long        _messageUntilMs = 0;

    bool    _forceFull      = true;
    uint8_t _lastBrightness = 0;
    bool    _lastFlip       = false;

    static void taskEntry(void *self);
    void taskLoop();
    // Draw one frame; returns the delay in ms before the next one.
    uint32_t renderFrame(unsigned long now);
    void beginNextCard(unsigned long now);
    void renderCurrentCard(unsigned long now);
    void renderAmbient(unsigned long now);
    void pollButton(unsigned long now);
    void crossFade(const PackedFrame &from, float k);
    ApproachStatus liveStatus(unsigned long now);   // dead-reckoned status of the current card
    void noteApproachProgress(Entry &e, const ApproachStatus &st, double prevAlt, unsigned long now);
    void startLanding(unsigned long now, bool demo);   // demo: replay only, card state unchanged
    bool renderLanding(unsigned long now);
    void startScene(unsigned long now, LandingScene::Kind kind, bool demo);
    // Showcase (display task only)
    bool          _showcaseActive = false;
    unsigned long _showcaseStartMs = 0;
    uint8_t       _showcaseStep = 0;
    void startShowcase(unsigned long now);
    void stepShowcase(unsigned long now);
    bool isFreshDeparture(const FlightInfo &f) const;
    bool isArrival(const Entry &e) const;
    float entryEta(const Entry &e, unsigned long now) const;
    int  dueIndex(unsigned long now, bool imminentOnly) const;
    bool screenAvailable(Screen s) const;
    void nextScreen();
    unsigned long screenDwellMs(Screen s) const;
    void renderMessage(const String &message);
    void applyPanelSettings();
    bool renderSpriteGallery();
    // Push the work frame to the panel, sending only changed pixels.
    void present();
};
