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
#include "display/ApproachModel.h"
#include "display/Traffic.h"
#include "display/DailyStats.h"
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

// Preview an ambient screen (1 = stats, 2 = clock) for durationMs. Safe from the web task.
void requestScreenPreview(uint8_t which, uint32_t durationMs);

// Replay the fly-across onto the current card. forceDirection: 0 = the
// flight's real direction, +1 = left-to-right, -1 = right-to-left (testing).
void requestFlyAcrossReplay(int forceDirection = 0);

// Replay the landing animation for the current card (testing / demo).
void requestLandingReplay();

// Play the go-around animation on the current card (demo).
void requestGoAroundDemo();

// Show the rare-spot flourish on the current card (demo).
void requestRareSpotDemo();

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

    // Frames drawn by the display task so far (heartbeat diagnostics).
    uint32_t framesDrawn() const { return _frames; }

    // Copy of the flight on screen, without its logo; false when none (scanning screen).
    bool currentFlight(FlightInfo &out);

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
        unsigned long altMs = 0;           // when shownAltFt was last updated
    };

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
    char                 _runwayInUse[4] = "";  // last arrival runway seen, e.g. "27L"
    Entry                _nextApproach;          // closest-to-touchdown aircraft not on the card
    bool                 _hasNextApproach = false;

    // Transition state (display task only)
    bool                 _inTransition = false;
    unsigned long        _transStartMs = 0;
    FlyAcross::Path      _path;            // from the incoming flight's track + vertical rate
    // Ambient screen (map / scanning) state (display task only)
    bool                 _ambientActive  = false;
    unsigned long        _ambientSinceMs = 0;
    unsigned long        _ambientUntilMs = 0;
    TrafficTracker       _traffic;               // guarded by _lock

    // Button-selected mode (display task only)
    enum class Mode : uint8_t { Auto, Map, Stats };
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

    bool                 _landingActive  = false;
    bool                 _landingDemo    = false;
    bool                 _landingIsGoAround = false;
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
    void crossFade(const FrameCanvas &from, float k);
    ApproachStatus liveStatus(unsigned long now);   // dead-reckoned status of the current card
    void noteApproachProgress(Entry &e, const ApproachStatus &st, double prevAlt, unsigned long now);
    void startLanding(unsigned long now, bool demo);   // demo: replay only, card state unchanged
    bool renderLanding(unsigned long now);
    void renderMessage(const String &message);
    void applyPanelSettings();
    bool renderSpriteGallery();
    // Push the work frame to the panel, sending only changed pixels.
    void present();
};
