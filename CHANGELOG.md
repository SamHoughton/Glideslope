# Changelog

What changed in each release. Firmware updates over Wi-Fi from the board's
web page (`firmware.bin`) keep your settings. Release files are on the
[releases page](https://github.com/SamHoughton/Glideslope/releases).

There is no 3.0: the features planned for it (living sky, holding stacks,
phone alerts) were finished alongside 4.0's and shipped together as 4.0.0.

## 4.1.0 (in progress)

Hardening: no new screens.

- **Web page code.** Requests must name the board (its address,
  `glideslope.local` or the setup hotspot), POSTs must come from the
  board's own page, and anything that changes the board (firmware, airport
  pack, settings, restart, Wi-Fi reset) needs a per-board code. The page
  asks for it once; a button shows it on the panel for 30 seconds.
- **Rollback.** A new firmware image is on trial until it has fetched
  positions (or run for five minutes); if it crashes before then, the
  board goes back to the previous image by itself.
- Settings changes are published in one step, so the main loop never sees
  half an update.
- Unit tests (approach model, runway and holding trackers, METAR and feed
  parsers, airport packs) and a replay of a recorded hour at Heathrow run
  on every push, with a static RAM budget.
- This changelog.

## 4.0.2 (18 Oct 2026)

- **Positions over HTTPS.** adsb.lol stopped answering plain HTTP, and the
  board fell back to adsb.fi and OpenSky with very little memory to spare.
  adsb.lol now uses one HTTPS connection kept open between fetches, with
  the reply read one aircraft at a time.
- **Fix:** an out-of-memory crash in the OpenSky fallback (it added to the
  previous fetch's list instead of replacing it).
- **Night sky:** a London skyglow and dithering instead of two flat bands;
  buildings darker than the glow with a moonlit edge.

## 4.0.1 (15 Oct 2026)

- **Fix:** a main-loop stack overflow (once in six days).
- Skyline readable in every light: deep indigo with a warm rim at sunset.
- Crash reports: `/api/status` and `/api/coredump`.

## 4.0.0 (9 Oct 2026)

- Living sky (day, golden hour, twilight, night with the real moon, cloud,
  rain, snow, fog) in the landing, go-around and take-off scenes.
- 20 shaded aircraft types in about 45 airline liveries.
- Weather scene, counting stats with yesterday's line, split-flap arrivals
  board with an optional departures page, night clock with the moon.
- Holding stacks; phone alerts through ntfy; runway tracker with
  direction-change announcements; brightness that can follow the sun.
- Routes from adsb.im over plain HTTP.

## 2.0.0 (8 Oct 2026)

- Any airport: about 50 ready-made airport packs (map, runways, time zone),
  installed from the web page; generated maps, Heathrow's included.
- Redesigned web page.

## 1.3.x (8 Oct 2026)

- Daily stats counted from every aircraft in range, saved across restarts.
- 1.3.1: fixed tables for the stats (memory).

## 1.2.x (7 Oct 2026)

- mDNS (`glideslope.local`), settings backup, browser installer.
- 1.2.1: memory fixes for TLS (plain-HTTP feeds, paced fetches).

## 1.1.0 and 1.0.0 (7 Oct 2026)

- First releases: approach cards, fly-across, landing, go-around and
  take-off scenes, map, arrivals board, weather, squawk alerts, OTA updates.
