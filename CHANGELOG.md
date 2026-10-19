# Changelog

What changed in each release. Firmware updates over Wi-Fi from the board's
web page (`firmware.bin`) keep your settings. Release files are on the
[releases page](https://github.com/SamHoughton/Glideslope/releases).

There is no 3.0: the features planned for it (living sky, holding stacks,
phone alerts) were finished alongside 4.0's and shipped together as 4.0.0.

## 5.0.0 (in progress)

The spotter's release. It includes the hardening planned as 4.1, which was
never released on its own.

**New**
- **City names** on routes (a switch on the web page): `LONDON > NEW YORK`
  on the cards, the departures board and the take-off caption, from an
  offline table of about 8,000 airports (OurAirports). Long routes read
  `FROM NEW YORK` when landing here, or scroll.
- **Route globe**: a new screen with the last flight's route on a globe,
  solid where it has been and dashed ahead, the night side shaded from the
  real sun, with distance flown and the ETA. It follows the landing in the
  showcase.
- **Stats**: today's page redesigned (tiles, shaded bars, the footer in the
  busiest airline's colours), then the top airlines and aircraft types.
- **Overhead**: an aircraft within a set distance of the home marker
  (2 km by default; overflights only unless switched) crosses the panel at
  a size set by its height, with contrails at cruising height.
- **Watchlist**: registrations, call signs, types or hex codes (`*` for a
  prefix) anywhere in range get the same pass with a `WATCHLIST` label, and
  a phone alert.
- **Registration history**: each card says how often the airframe has been
  seen and when last (`SEEN 14 TIMES`, `LAST ON 3 OCT`, `FIRST VISIT`),
  kept in a 16 KB table in flash.
- **Alerts with a picture**: emergency, rare-spot and watchlist alerts
  attach the panel as it is when they go out.
- **Home Assistant** over MQTT with discovery: next arrival and ETA,
  runways in use, today's movements, aircraft holding, the last flight and
  the last aircraft overhead.
- **Through the year**: snow on the roofs when freezing, Christmas lights
  in December, fireworks on Bonfire Night and New Year's Eve.

**Hardening**
- **Web page code.** Requests must name the board (its address,
  `glideslope.local` or the setup hotspot), POSTs must come from the
  board's own page, and anything that changes the board (firmware, airport
  pack, settings, restart, Wi-Fi reset) needs a per-board code. The page
  asks for it once; a button shows it on the panel for 30 seconds.
- **Signed releases.** Release firmware is signed in CI; the board checks
  the signature, and your own unsigned builds install after a warning.
- **Rollback.** A new firmware image is on trial until it has fetched
  positions (or run for five minutes); if it crashes before then, the
  board goes back to the previous image by itself.
- **More memory.** The frame a transition starts from is kept at 12 bits a
  pixel: 4 KB more free with the feed's connection open.
- Settings changes are published in one step, so the main loop never sees
  half an update.
- Unit tests (approach model, runway and holding trackers, METAR and feed
  parsers, airport packs, airport table, spotter, registration history) and
  a replay of a recorded hour at Heathrow run on every push, with a static
  RAM budget. `tools/host_build.py` runs them, and renders screens to
  images, on a desktop without a toolchain.
- This changelog.

## 4.0.2 (18 Oct 2026)

The open ADS-B feeds changed under the board this month, and 4.0.0 and
4.0.1 crashed on the fallbacks they were pushed onto.

- **adsb.fi is now the primary feed.** adsb.lol stopped answering plain
  HTTP, then started allowing 500 requests a day per address. adsb.fi is
  fetched over one HTTPS connection kept open between fetches, with each
  reply read one aircraft at a time; adsb.lol is the backup.
- **Fix:** out-of-memory crashes in the fallbacks. OpenSky added to the
  previous fetch's list instead of replacing it, held its whole reply
  twice, and its chunked replies weren't decoded. The aircraft lists are
  now capped and reserved up front, after checking there is room.
- **Fix:** out-of-memory crashes saving the stats and reserving the
  aircraft list with the feed connection open.
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
