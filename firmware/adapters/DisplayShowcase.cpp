/*
Purpose: The showcase (the scripted tour for recordings) and the sprite
gallery. Part of NeoMatrixDisplay.
*/
#include "adapters/DisplayInternal.h"

void NeoMatrixDisplay::startShowcase(unsigned long now)
{
    // GS101, a fictional flight in the Glideslope badge: 3.2 km out on final
    // for 27L (east of the threshold, tracking west), 1,050 ft, descending.
    Entry e;
    FlightInfo &f = e.flight;
    f.ident = "GSL101";
    f.ident_icao = "GSL101";
    f.ident_iata = "GS101";
    f.operator_icao = "GSL";
    f.airline_display_name_full = "Glideslope";
    f.origin.code_iata = "JFK";       f.origin.code_icao = "KJFK";
    f.destination.code_iata = "LHR";  f.destination.code_icao = "EGLL";
    f.aircraft_code = "A35K";
    f.aircraft_display_name_short = "A350-1000";
    f.registration = "G-GSLP";
    f.airline_logo_rgb565.assign(kDemoLogo, kDemoLogo + 32 * 32);
    const double kmEast = 3.2;
    f.lat = 51.4649;
    f.lon = -0.4340 + kmEast / (111.32 * cos(51.4649 * M_PI / 180.0));
    f.heading = 269.7;
    f.velocity = 148;
    f.baro_altitude = 1050;
    f.vertical_rate = -760;
    f.distance_km = 6;
    e.dataMs = now;
    e.accent = Rgb{255, 185, 60};   // the brand amber

    g_oldFrame.clear();   // fly in over a clear panel: no real airline's logo in a recording
    _ambientActive = false;
    _landingActive = false;
    _alertActive = false;
    _current = e;
    _hasCurrent = true;
    _inTransition = true;
    _transStartMs = now;
    _path = FlyAcross::pathFor(f.heading, f.vertical_rate, g_config.screen_facing);
    _showcaseActive = true;
    _showcaseStartMs = now;
    _showcaseStep = 0;
    Log.println("Display: showcase");
}

// Timeline from the start: card (with its countdown) to 9 s, landing, the
// LANDED card, then the globe 5 s, map 5 s, arrivals 5 s, weather 5 s,
// stats 4.5 s, holding 4.5 s, a take-off, done.
void NeoMatrixDisplay::stepShowcase(unsigned long now)
{
    const uint32_t t = now - _showcaseStartMs;
    switch (_showcaseStep)
    {
        case 0: if (t >= 9000)  { startLanding(now, false); ++_showcaseStep; } break;
        case 1: if (t >= 15500) { noteGlobe(_current); s_screenPreviewUntil = now + 5000; s_screenPreview = 6; ++_showcaseStep; } break;
        case 2: if (t >= 20500) { s_screenPreview = 0; s_mapPreviewUntil = now + 5000; ++_showcaseStep; } break;
        case 3: if (t >= 25500) { s_mapPreviewUntil = 0; s_screenPreviewUntil = now + 5000; s_screenPreview = 3; ++_showcaseStep; } break;
        case 4: if (t >= 30500) { s_screenPreviewUntil = now + 5000; s_screenPreview = 4; ++_showcaseStep; } break;
        case 5: if (t >= 35500) { s_screenPreviewUntil = now + 4500; s_screenPreview = 1; ++_showcaseStep; } break;
        case 6: if (t >= 40000) { s_screenPreviewUntil = now + 4500; s_screenPreview = 5; ++_showcaseStep; } break;
        case 7: if (t >= 44500)
                {
                    s_screenPreview = 0;
                    _ambientActive = false;
                    startScene(now, LandingScene::Takeoff, true);
                    ++_showcaseStep;
                }
                break;
        default:
            if (t >= 44500 + LandingScene::DURATION_MS + 1500)
            {
                _showcaseActive = false;
                _hasCurrent = false;   // back to normal service
                Log.println("Display: showcase over");
            }
            break;
    }
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
    // Nine types a page, a page every 3 s, each in a different airline's colours.
    static const char *const kAirlines[] = {"BAW", "VIR", "EZY", "RYR", "KLM", "UAE", "DLH", "AFR", "QTR"};
    const int page = (now / 3000) % ((AircraftSprites::KindCount + 8) / 9);
    for (int i = 0; i < 9; ++i)
    {
        const int k = page * 9 + i;
        if (k >= AircraftSprites::KindCount) break;
        const AircraftSprites::Sprite &sp = AircraftSprites::get((AircraftSprites::Kind)k);
        FlightInfo f;
        f.ident = kAirlines[i];
        const int cx = (i % 3) * 43, cy = (i / 3) * 21;
        AircraftSprites::draw(g_workFrame, sp, cx + (42 - sp.w) / 2, cy + (21 - sp.h) / 2,
                              AircraftSprites::liveryFor(f, Rgb{90, 170, 255}));
    }
    present();
    return true;
}
