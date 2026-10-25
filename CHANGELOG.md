# Changelog

What changed in each release. Firmware updates over Wi-Fi from the board's
web page (`firmware.bin`) keep your settings. Release files are on the
[releases page](https://github.com/SamHoughton/Glideslope/releases).

There is no 3.0: the features planned for it (living sky, holding stacks,
phone alerts) were finished alongside 4.0's and shipped together as 4.0.0.

## 8.4.0 (25 Oct 2026)

**New**
- **How long it has been flying**: the route globe shows, in turn with the
  distance, how long the flight has been in the air (`13h 50m flying`), and
  once landed the whole flight (`12h 40m flight`). ADS-B carries no take-off
  time, so it is worked out from the distance flown at an average that
  matches real block times (Singapore to London about 13h 50m, New York
  7h); the origin page uses the same estimate.

**Fixes**
- The log keeps what matters: the fetcher no longer writes three lines
  every five seconds (cache hits, the nearest flight, the count), which
  pushed everything else (go-arounds, landings, weather) out of the 36-line
  log within a minute. It now says when the nearest flight or the count
  changes.
- A crash on a busy day: the map's airline colours loaded each logo whole
  (2 KB) to pick its colour, and with the memory fragmented one load failed
  and the board restarted. The colour is now read from the logo's file a
  little at a time, 80 airlines are remembered (64, and none after that),
  and a card whose logo can't be loaded shows without it.
- The go-around alert is logged when it plays (or if it is dropped).

## 8.3.0 (24 Oct 2026)

**New**
- **Go-arounds you can't miss**: one anywhere in range now takes over the
  panel for ten seconds: a flashing amber frame, GO-AROUND, the flight, the
  runway and height, and its track climbing away. (Before, only the
  aircraft on the card played its scene; the rest went to the spotter log
  and your phone.) Day and night, after any landing already playing. A
  demo under Animations, More.
- **A route memory**: every route the board learns is kept in its flash
  (about 5,000 of them), and fills in when the online sources come up
  empty: a regular like VIR156T (Las Vegas, almost daily), which adsb.im
  doesn't know, only has to be found once.
- **More rare spots**: the A340, MD-11, DC-10, 757, A318, A300/A310,
  BAe 146 and Avro RJ, Il-76 and Il-96, Beluga XL and Dreamlifter, with the
  A380, 747, An-124 and Concorde.

**Fixes**
- **Real heights near the airport**: aircraft report their height against
  standard pressure, so on a low-pressure day they seemed to land at 500 ft
  or more (at 991 hPa, Heathrow's runway reads about 680 ft). Below 6,000 ft
  heights are now corrected to the airport's pressure from the METAR, as a
  pilot's altimeter is; flight levels above stay as they are.
- The next aircraft to land always gets the card: one no route database
  knew (VIR156T) was dropped and a later arrival shown instead. Without a
  route its card shows the airline.
- A go-around by the aircraft on the card could go unshown; it now always
  gets the alert.
- The next to land without a route is looked up again after 30 seconds
  rather than 5 minutes.

## 8.2.1 (24 Oct 2026)

**Fixes**
- Updates at a tap could stop part-way on a board that had been up for an
  hour or two (its memory too fragmented for a 1.7 MB download). The board
  now restarts first and downloads the update straight after, before
  anything else starts; it is installed in well under a minute.

## 8.2.0 (24 Oct 2026)

The airfield's moods, and a board that looks after itself.

**New**
- **Low visibility and thunderstorms**: the board reads the cloud base and
  thunder from the METAR. Fog or a low ceiling (under 1,000 m, or cloud at
  200 ft or below) gets a banner with fog drifting by; a thunderstorm over
  the airfield or nearby gets one with rain and lightning. Both go to your
  phone (a new alert kind, Fog and storms, on by default), and the weather
  screen says "Low visibility" or "Thunderstorm" in place of the weather words.
- **The runway outlook**: the weather request now brings the airport's TAF
  too. When the forecast wind would put 5 kt or more of tailwind on the
  runways in use (at Heathrow on easterlies: when it lets them go back
  west), the weather screen shows `To 09 at 19:00`, the web page says so,
  and a change within three hours gets a yellow-on-black sign on the panel
  and an alert on your phone.
- **Updates at a tap**: the System tab checks GitHub for a newer release
  and installs it with one button. The board downloads the release itself
  and only takes a signed one; it restarts into it in under a minute.
- **A health page**: a Health tab with the board's memory over the last
  24 hours, every start and why it happened (an update, the restart button,
  a self-heal, a crash, a power cut), how the feed and the weather are
  doing, the Wi-Fi signal, storage and the chip's temperature.
- Demos for the new banners under Animations, More.

**Changes**
- The log on the web page keeps the last 36 lines (40), for the room the
  health history takes.

## 8.1.0 (24 Oct 2026)

Night watch, the logbook, and a steadier board.

**New**
- **Night watch** (on by default, Night tab): through the night hours
  only a quiet view out of the window. The skyline under the real sky,
  the time, and every aircraft as just its lights where it really is
  (its bearing against the way the window faces, up by its angle above
  the horizon), with a dim tally of the night. No cards, scenes or
  banners; emergencies still take over.
- **The logbook**: every day, every go-around and rare sighting, and
  every airline and type the board has ever seen, kept in its flash for
  good. A **History** tab on the web page with the records, a heatmap of
  busy days, the moments and the collection, and CSV downloads. A new
  record or a collection milestone gets a short gold banner, and today
  beating the busiest day is announced as it happens.
- **Go-arounds from every aircraft** in range, not just the one on the
  card: counted, logged and sent to your phone.
- **Halloween** (18-31 October): jack-o'-lanterns along the skyline,
  bats after dusk, pumpkin-lit windows.
- **"Last seen 4 days ago"** on the card, in turn with how often the
  aircraft has been seen.
- **Curved routes on the globe**: it now looks at the route from the
  equator side, so a flight's great circle bends towards the pole as it
  does on a flight map (from straight above, it looked like a straight line).
- **The live preview only when asked for**: the web page shows a still of
  the panel; the Live button (or a demo) shows it moving for two minutes.

**Fixes**
- The board could crash when a phone had the web page open (it ran out
  of memory during a fetch); the fetch and the web server now take turns,
  and a failed allocation skips a fetch instead of ending the program.
- It could stay on the scanning screen with its memory too fragmented
  for the feed: the self-heal restart now fires (it needed more free
  memory than the board has since 8.0), 3.3 KB more memory is free, and
  the extra weather requests for flights' origins are gone (the origin
  page uses the regular weather request's).
- The spotter log keeps go-arounds, emergencies, watchlist hits, rare
  types and first visits when overhead passes pile up, and aircraft being
  turned onto final no longer count as overhead.
- Up to 96 airlines a day are counted (48 before); airlines seen without
  a logo are remembered across restarts.

## 8.0.0 (20 Oct 2026)

The line-up: everyone coming in, where they came from, where they're going.

**New**
- **Approach line-up**: the glide path seen side on, the threshold on
  the left and 27 km out on the right, every aircraft on final at its
  real distance and height in its airline's colour, and the time to the
  one ahead under it (red under a minute). In the rotation when two or
  more are coming in, and a page on final.
- **Where it came from**, a page on final: the sky at the origin as it is
  now (its own sun, weather and moon) with its temperature, its local time
  and weather in words, and roughly how long the flight has been flying
  and how far. The 10-minute weather request asks for the next arrivals'
  origins too, so it costs no extra connection.
- **Departure headings**, a third page of the board when departures are
  on: a compass round the airport with a ray along each recent
  departure's course to its destination.

**Fixes**
- Over a busy hour or so the free memory became too fragmented for a
  secure connection and the board restarted itself (7.x too, more
  slowly). The route and airline-colour caches are fixed tables now, and
  airport codes come from the offline table, so the largest free block
  holds steady.

## 7.1.0 (20 Oct 2026)

The day and the sky.

**New**
- **Runway rhythm**, a stats page: a landing every 1:45 (from the last
  eight gaps), today's average and tightest gap with the aircraft, and
  each runway's share of the arrivals.
- **Spotter log**: today's interesting sightings (rare spots, first
  visits, watchlist and overhead passes, go-arounds, emergencies) as a
  stats page, and on the web page in a new **Today** card with the
  movements and the rhythm. The last 16 are kept, saved with the stats.
- **Bedtime summary**: overnight, for 15 s a minute, the day in one card
  (movements, busiest hour, tightest gap, the best spot).
- **Night scene** instead of the plain clock: the skyline under the real
  sky (stars, the moon in its phase, lit windows, the season), distant
  runway lights, the time above, the date in turn with the next sunrise
  and the weather, and now and then an aircraft's lights far off.
- **Sunrise and sunset** on the weather screen (`Sunset in 24 min` inside
  the hour).

## 7.0.0 (20 Oct 2026)

The look: new type, one design across the screens, better aircraft.

**New**
- **New type**: big numbers and flight numbers in Barlow with softened
  edges, and a slim pixel font with lower case for the rest, in sentence
  case (`Approach 27L`, `New York → London`, `Light rain`).
- **One design**: every screen shares a title band, palette and spacing;
  the card, stats, weather, clock, aircraft page, globe, alerts and
  captions are redrawn in it.
- **Split-flap boards**: arrivals and departures on yellow flaps that run
  through the alphabet and land; arrivals show where each flight is from
  and the time it is expected to land (`LAND` inside 90 s).
- **Weather in words** along the boards and under the clock:
  `11°  light rain  250/14G26`.
- **Aircraft**: the A319, A320neo, A321neo, 737 MAX and 777X are told
  apart (engines, winglets, tail cone); winglets and sharklets; the beacon
  flashes on the card; flaps out on the approach and climb-out; heat haze
  behind the engines at take-off power; freighters without windows.
- **Map icons by size**: regional jets and props small, widebodies large.

**Fixes**
- The board restarted (watchdog) when a phone stopped reading the live
  preview without closing it, e.g. when its screen locked; a reply now
  gives up after 8 s.

## 6.0.0 (19 Oct 2026)

Polish: the animations, alerts and logos.

**New**
- **Airline logos from the web page**: the Display tab lists the airlines
  seen without a logo and adds one (the browser fits and converts the
  image, with a 32x32 preview); a bad one can be removed. No USB needed.
- **The approach strip** along the bottom of the card is a glide path
  with a small aircraft coming down it to the runway, instead of an arrow.
- **Runway change**: the number flips on split-flap tiles, a windsock
  swings round, and a runway from above shows the new landing end.
- **Map to card**: a card arriving while the map is up zooms in on the
  aircraft's icon.
- **Weather on the scenes**: landing-light beams through rain, snow and
  fog; spray and a reflection on a wet runway; the gear tucking up after
  take-off.
- **Crosswind**: a windsock by the threshold in the real wind, and
  `XWIND 17G26` on the approach with 10 kt or more across the runway.
- **Quiet hours** for phone alerts (23:00 to 07:00 by default): only
  emergencies get through.

**Fixes**
- `FIRST VISIT` waits until the registration history is a week old (at
  first nearly every aircraft is new to it).
- The aircraft page shortens long airline names by whole words.
- The route globe's night side is readable.

## 5.1.0 (19 Oct 2026)

**New**
- **Map**: flight numbers beside the aircraft, a spotlight cycling through
  them with flight, type and height, and trails of the last few minutes.
- **More on final**: an arrival's card takes turns with its route on the
  globe and a page about the aircraft; back on the card for good 40 s out.

**Fixes**
- The departures board with city names ran a long city into the time;
  the weather screen drew gusts over the wind. Both found by the new
  screen tests.
- Firmware uploads wait for the feed to pause, so they don't stall.
- **The flight feed no longer drops out.** Under busy traffic (5.0.0 too)
  the board restarted every 10-15 minutes: whenever the feed's HTTPS
  connection was closed (for the weather, every 10 minutes) the heap was
  too broken up to open it again. A TLS arena now keeps room for the
  connection's two 16 KB buffers from start-up; the two aircraft lists take
  turns instead of a new one every 5 seconds; a fetch is no longer skipped
  for room the list already has; the stats save never closes the feed;
  and if the feed still can't reopen, the board saves its stats and
  restarts in seconds. Soak: 47 minutes, no restarts, the feed back after
  every weather update.
- The route globe zooms in on short routes, and has no blue rim.
- Crash reports say whether the dump came from the running build.
- Web requests: Content-Length without a space after the colon, and query
  keys matched whole.

**Under the hood**
- Every screen is rendered on each push and compared with a reference
  image (`tools/host_build.py golden`); the request parser is fuzzed.
- The display code is split by job (rhythm, scenes, showcase).
- CI actions on Node 24.

## 5.0.0 (19 Oct 2026)

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
