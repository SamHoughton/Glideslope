# Test fixtures

`heathrow.jsonl`: 19 minutes of traffic around Heathrow on 18 October 2026,
as [adsb.lol](https://adsb.lol/) served it every 10 seconds (one JSON line
per fetch, `{"t": unix time, "ac": [readsb aircraft]}`), trimmed to aircraft
below 6,000 ft within 25 km. Data from adsb.lol, licensed under the
[ODbL](https://opendatacommons.org/licenses/odbl/).

`heathrow.expected`: the number of landings in it, counted independently of
the firmware's approach model: aircraft seen below 1,500 ft, descending,
within 8 km of the airport and tracking within 25 degrees of east or west.
The replay test (`test_replay_counts_arrivals`) runs the recording through
the board's stats and runway tracker and checks its arrival count is within
15% of that.

Don't record new fixtures from the same network as a running board: the
recorder and the board share adsb.lol's rate limit.
