#!/usr/bin/env python3
"""
Serve the board's web page on this computer with made-up data, for
screenshots and for working on the page without a board.

  python tools/webui_preview.py                 # http://127.0.0.1:8765/
  python tools/webui_preview.py --shots         # docs/webui-desktop.png, -alerts, -spotter, -phone

The page is firmware/web/index.html with the brand mark filled in, exactly as
the board serves it. The API answers with
sample settings (Heathrow, no keys), a sample flight and log, and a panel
frame from the last tools/record_demo.py recording (else a blank panel).
Screenshots use Chrome or Edge in headless mode.
"""
import argparse
import json
import pickle
import re
import shutil
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PORT = 8765


def page_html():
    """The page exactly as the board serves it (scripts/gen_webpage.py)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location('gen_webpage', ROOT / 'firmware' / 'scripts' / 'gen_webpage.py')
    mod = importlib.util.module_from_spec(spec)
    import io, contextlib
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(mod)
    return mod.page()


CONFIG = {
    'tar1090_host': '', 'center_lat': 51.47, 'center_lon': -0.4543, 'home_lat': 51.5007, 'home_lon': -0.1246,
    'radius_km': 20, 'min_altitude_ft': 100, 'display_brightness': 128,
    'text_color_r': 255, 'text_color_g': 255, 'text_color_b': 255,
    'display_nearest_only': False, 'display_border': True, 'show_flight_number_with_logo': True,
    'show_aircraft_type': True, 'show_aircraft_registration': False, 'swap_aircraft_type_reg': False,
    'screen_facing': 'S', 'display_flip': False, 'night_mode_enabled': True,
    'night_start_minutes': 1320, 'night_end_minutes': 420, 'night_brightness': 38, 'utc_offset_minutes': 0,
    'fetch_interval_seconds': 30, 'local_fetch_interval_seconds': 5, 'display_cycle_seconds': 10,
    'card_lead_seconds': 120, 'interlude_seconds': 15, 'screens': 95, 'route_cities': True,
    'aeroapi_cache_ttl_seconds': 1800, 'aeroapi_fail_cache_ttl_seconds': 300,
    'opensky_client_id': '', 'opensky_client_secret': '', 'opensky_priority': False,
    'use_community_feeds': True, 'aeroapi_key': '', 'night_follow_sun': True,
    'ntfy_topic': 'glideslope-k3x9q2m7wt', 'ntfy_token': '', 'notify_mask': 127,
    'quiet_alerts': True, 'quiet_start_minutes': 1380, 'quiet_end_minutes': 420, 'overhead_km': 2.0, 'overhead_all': False, 'watchlist': 'G-CIVA, B74*, A388, RRR*',
    'mqtt_host': '192.168.1.20', 'mqtt_port': 1883, 'mqtt_user': 'glideslope', 'mqtt_pass': '***',
}
STATUS = {
    'version': 'v8.1.0', 'built': 'Oct 24 2026 10:00:00', 'uptime_s': 86400, 'last_reset': 'power on',
    'heap_free': 84000, 'heap_max_block': 55000, 'heap_min_free': 18000, 'display_frames': 1700000,
    'web_requests': 5200, 'metar': 'METAR EGLL 071650Z 36011KT 9999 FEW030 12/06 Q1012 NOSIG',
    'airport': 'EGLL', 'airport_name': 'HEATHROW', 'airport_lat': 51.47, 'airport_lon': -0.4543, 'airport_pack': False,
    'runways_arr': '27L', 'runways_dep': '27R', 'holding': 'BNN 4 (9 min), OCK 3 (7 min)',
    'mood': 'Low visibility', 'outlook': 'Runway 09 from about 19:00 (wind 070/12)', 'update_error': '',
}
# What GitHub's API says about the newest release (the page asks it directly).
RELEASE = {
    'tag_name': 'v8.2.0', 'published_at': '2026-10-26T10:00:00Z',
    'body': 'Glideslope 8.2.0\n\nFog, storms and the runway outlook; updates at a tap and a health page.\n\n**New**\n- ...',
}


def health():
    import math
    free = [max(14, round(27 + 4 * math.sin(i / 7) - (6 if 40 < i < 46 else 0))) for i in range(96)]
    block = [max(8, round(16 + 4 * math.sin(i / 5 + 1) - (5 if 40 < i < 46 else 0))) for i in range(96)]
    return {
        'version': 'v8.1.0', 'built': 'Oct 24 2026 10:00:00', 'uptime_s': 266400, 'boot': 'update to v8.1.0 (GitHub)',
        'last_reset': 'restart', 'boot_count': 41,
        'boots': [{'at': 1792846800, 'why': 'update to v8.1.0 (GitHub)'},
                  {'at': 1792790400, 'why': 'restart from the web page'},
                  {'at': 1792702200, 'why': 'self-heal: memory fragmented'},
                  {'at': 1792444800, 'why': 'power on'}],
        'heap': {'free': 26400, 'block': 15860, 'min_free': 6060, 'every_min': 15, 'free_kb': free, 'block_kb': block},
        'feed': {'ok': 9874, 'failed': 112, 'last_ok_s': 4}, 'weather': {'ok': 444, 'failed': 3, 'last_ok_s': 312},
        'wifi': {'rssi': -61, 'channel': 6}, 'storage': {'used': 401408, 'total': 1441792, 'logbook': 19456},
        'temp_c': 47.5, 'tls': {'served': 1210, 'to_heap': 0},
    }
DISPLAY = {
    'active': True, 'ident': 'GSL101', 'flight': 'GS101', 'airline_name': 'Glideslope', 'origin': 'JFK',
    'dest': 'LHR', 'aircraft_name': 'A350-1000', 'registration': 'G-GSLP', 'altitude_ft': 1010,
    'speed_kt': 148, 'heading_deg': 270, 'vspeed_fpm': -760,
}
LOG = [
    '[16:58:02] Fetch (adsb.lol): 14 aircraft in range, showing: GSL101',
    '[16:58:02]   GS101 (GSL101) JFK>LHR A350-1000, 1050 ft, 148 kt, hdg 270',
    '[16:58:03] Display: fly-across to GSL101, right-to-left, descending (0 still queued)',
    '[16:58:30] Weather: METAR EGLL 071650Z 36011KT 9999 FEW030 12/06 Q1012 NOSIG -> "360/11 10KM+ 12C"',
    '[16:58:41] Display: GSL101 touchdown on 27L',
    '[16:58:53] Display: break after landing, arrivals',
    '[16:59:02] Beat 86400s: +1187 frames, +38 web, heap 84112 blk 55284 low 18012, 6 routes',
]


def rle(frame):
    """The board's ?z=1 frame encoding: [count 1-255][pixel lo][pixel hi] per run."""
    px = [frame[i:i + 2] for i in range(0, len(frame), 2)]
    out, i = bytearray(), 0
    while i < len(px):
        run = 1
        while i + run < len(px) and run < 255 and px[i + run] == px[i]:
            run += 1
        out += bytes([run]) + px[i]
        i += run
    return bytes(out)


def sample_frame():
    # The approach card from the last showcase recording: (showcase ms, frame) pairs.
    cache = ROOT / 'tools' / '.cache' / 'showcase_frames.pickle'
    if cache.exists():
        samples = pickle.loads(cache.read_bytes())
        px = min(samples, key=lambda s: abs(s[0] - 6000))[1]
        import struct
        return struct.pack('<8192H', *px)
    return bytes(16384)

# A phone-width view: the page in a 390 px frame (headless Chrome won't make a
# window that narrow), cropped afterwards.
PHONE = b'''<!doctype html><html><body style="margin:0;background:#0b0e13">
<iframe src="/" style="border:0;width:390px;height:1100px;display:block"></iframe></body></html>'''


class Handler(BaseHTTPRequestHandler):
    html = b''
    frame = b''

    def log_message(self, *a):
        pass

    def send(self, body, ctype='application/json'):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path.split('?')[0]
        if p == '/':                 self.send(self.html, 'text/html; charset=utf-8')
        elif p == '/phone':          self.send(PHONE, 'text/html; charset=utf-8')
        elif p == '/logos':          self.send(self.html.replace(b'</body>', b'<script>(function(){var t=document.getElementById("t-display");for(var e=t.firstElementChild;e&&e.textContent.indexOf("Airline logos")<0;e=e.nextElementSibling)e.style.display="none";})()</script></body>'), 'text/html; charset=utf-8')
        elif p == '/api/config':     self.send(CONFIG)
        elif p == '/api/status':     self.send(STATUS)
        elif p == '/api/health':     self.send(health())
        elif p == '/mock/release':   self.send(RELEASE)
        elif p == '/api/logos':      self.send({'count': 177, 'missing': [{'code': 'CTN', 'seen': 9}, {'code': 'LZB', 'seen': 4}, {'code': 'RBA', 'seen': 2}]})
        elif p == '/api/display':    self.send(DISPLAY)
        elif p == '/api/today':      self.send(TODAY)
        elif p == '/api/history/records': self.send(HIST_RECORDS)
        elif p == '/api/history/days':       self.send(hist_days().encode(), 'text/csv')
        elif p == '/api/history/events':     self.send(HIST_EVENTS.encode(), 'text/csv')
        elif p == '/api/history/collection': self.send(HIST_COLLECTION.encode(), 'text/csv')
        elif p == '/api/history/airframes':  self.send(HIST_AIRFRAMES.encode(), 'text/csv')
        elif p == '/api/logo':       self.send_error(404)   # no airline logos in the docs (trademarks)
        elif p == '/api/traffic':    self.send(TRAFFIC)
        elif p == '/api/frame':      self.send(rle(self.frame) if 'z=1' in self.path else self.frame, 'application/octet-stream')
        elif p == '/api/log':        self.send({'cursor': len(LOG), 'lines': LOG if 'cursor=0' in self.path else []})
        elif p.startswith('/packs/') and (ROOT / p.lstrip('/')).is_file():
            # The repo's packs/, for trying the airport picker locally
            # (in the browser console: PACKS='/packs/' then reload the list).
            self.send((ROOT / p.lstrip('/')).read_bytes(),
                      'application/json' if p.endswith('.json') else 'application/octet-stream')
        else:                        self.send_error(404)

    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(n) if n else b''
        if self.path == '/api/airport':
            head = body.split(b'\n', 1)[0]
            try:
                self.send({'ok': True, 'airport': json.loads(head)['icao'], 'bytes': len(body)})
            except Exception:
                self.send({'ok': False, 'error': 'header is not JSON'})
            return
        self.send({'ok': True})


# Today (7.1): the runway rhythm and the spotter log, a busy day.
TODAY = {'arrivals': 612, 'departures': 598, 'every': 105, 'average': 128, 'tightest': 72,
         'tightest_ident': 'BAW117', 'tightest_at': 492, 'busiest_hour': 7, 'busiest_n': 88, 'spotted': 7,
         'runways': [{'name': '27L', 'n': 402}, {'name': '27R', 'n': 210}],
         'spots': [{'at': 1196, 'kind': 'first', 'ident': 'SQ322', 'what': '9V-SMF'},
                   {'at': 1180, 'kind': 'overhead', 'ident': 'EI158', 'what': 'A20N'},
                   {'at': 1142, 'kind': 'watch', 'ident': 'G-CIVA', 'what': 'B744'},
                   {'at': 1068, 'kind': 'goaround', 'ident': 'BA117', 'what': '27L'},
                   {'at': 862, 'kind': 'overhead', 'ident': 'LH915', 'what': 'A20N'},
                   {'at': 665, 'kind': 'first', 'ident': 'VS3', 'what': 'G-VJAZ'},
                   {'at': 581, 'kind': 'rare', 'ident': 'QR7', 'what': 'A388'}]}


# History (8.1): a made-up logbook, about four months of it.
HIST_RECORDS = {'since': '2026-06-25', 'days': 120, 'events': 214, 'bytes': 19800, 'best_day': 1312,
                'best_day_date': '2026-08-14', 'best_go_arounds': 6, 'best_go_arounds_date': '2026-09-28',
                'tightest': 58, 'tightest_date': '2026-08-02', 'airlines': 143, 'types': 61}


def hist_days():
    import datetime, math, random
    random.seed(7)
    out = ['date,arrivals,departures,go_arounds,busiest_airline,busiest_n,tightest_s,average_gap_s,runway,runway_n,airlines,types' +
           ''.join(',h%02d' % h for h in range(24))]
    d = datetime.date.today() - datetime.timedelta(days=120)
    for i in range(120):
        n = int(560 + 60 * math.sin(i / 9) + random.randint(-40, 40) + (40 if d.weekday() in (4, 6) else 0))
        out.append('%s,%d,%d,%d,BA,%d,%d,%d,27L,%d,%d,%d' % (d.isoformat(), n, n - 10, random.choice([0, 0, 0, 1, 2]),
                                                          n // 2, random.randint(60, 90), random.randint(95, 130), n - 20, 70, 38)
                   + ',0' * 24)
        d += datetime.timedelta(days=1)
    return '\n'.join(out) + '\n'


HIST_EVENTS = """time,kind,flight,detail
2026-09-28 16:42,go-around,BA117,27L
2026-10-02 09:41,rare,QR7,A388
2026-10-11 14:22,watchlist,G-CIVA,B744
2026-10-19 07:55,first visit,SQ322,9V-SMF
2026-10-22 21:03,go-around,VS4,27R
2026-10-23 18:36,rare,EK4CK,A388
"""
TRAFFIC = {
    'aircraft': [
        {'id': '406b62', 'f': 'BA311', 't': 'A320', 'lat': 51.4655, 'lon': -0.3200, 'h': 270, 'gs': 140, 'alt': 1300, 'c': '#5a8cff', 's': 1, 'fin': 85, 'rwy': '27L'},
        {'id': '4ca9ba', 'f': 'EI158', 't': 'A20N', 'lat': 51.4650, 'lon': -0.1550, 'h': 270, 'gs': 160, 'alt': 2700, 'c': '#3cc878', 's': 1, 'fin': 200, 'rwy': '27L'},
        {'id': '896183', 'f': 'EK1', 't': 'A388', 'lat': 51.4660, 'lon': 0.0300, 'h': 268, 'gs': 180, 'alt': 4400, 'c': '#e03030', 's': 2, 'fin': -1, 'rwy': ''},
        {'id': '4007f2', 'f': 'BA117', 't': 'B77W', 'lat': 51.4780, 'lon': -0.6200, 'h': 275, 'gs': 210, 'alt': 3800, 'c': '#5a8cff', 's': 2, 'fin': -1, 'rwy': ''},
        {'id': '3c66b1', 'f': 'LH915', 't': 'A20N', 'lat': 51.6900, 'lon': -0.5300, 'h': 120, 'gs': 220, 'alt': 8000, 'c': '#ffcc33', 's': 1, 'fin': -1, 'rwy': ''},
        {'id': '4ca8f1', 'f': 'VS3', 't': 'A35K', 'lat': 51.6200, 'lon': 0.1000, 'h': 300, 'gs': 230, 'alt': 9000, 'c': '#d02040', 's': 2, 'fin': -1, 'rwy': ''},
    ],
    'airport': {'icao': 'EGLL', 'lat': 51.4700, 'lon': -0.4543,
                'runways': [['27L', 51.4648, -0.4342, 269.7], ['27R', 51.4777, -0.4330, 269.7],
                            ['09L', 51.4775, -0.4850, 89.7], ['09R', 51.4645, -0.4823, 89.7]],
                'holds': [['BNN', 51.7261, -0.5498], ['LAM', 51.6461, 0.1517], ['BIG', 51.3311, 0.0328], ['OCK', 51.3050, -0.4475]]},
}

HIST_AIRFRAMES = "registration,type,airline,first_seen,last_seen,days\n" + "".join(
    f"G-X{i:03d},{t},{op},2026-10-25,2026-10-25,1\n" for i, (t, op) in enumerate(
        [("A320", "BAW")] * 40 + [("A20N", "BAW")] * 25 + [("A35K", "BAW")] * 12 + [("B789", "VIR")] * 9 +
        [("A388", "UAE")] * 7 + [("B77W", "UAE")] * 5 + [("A359", "SIA")] * 3))

HIST_COLLECTION = """kind,code,first_seen,last_seen,days,count
airline,AF,2026-06-27,2026-10-23,110,2210
airline,KL,2026-06-25,2026-10-23,118,2050
airline,SQ,2026-07-02,2026-10-23,90,480
airline,UA,2026-06-25,2026-10-23,120,1500
airline,DL,2026-06-26,2026-10-23,119,1300
airline,QF,2026-07-14,2026-10-23,80,160
airline,B6,2026-08-03,2026-10-23,70,140
airline,NH,2026-09-12,2026-10-20,20,40
airline,KQ,2026-10-02,2026-10-22,12,24
airline,BG,2026-10-24,2026-10-24,1,1
type,A388,2026-06-25,2026-10-23,120,3200
type,B744,2026-07-30,2026-10-22,15,20
type,A306,2026-10-24,2026-10-24,1,1
type,B38M,2026-06-28,2026-10-23,110,900
type,A359,2026-06-25,2026-10-23,120,1200
type,BCS3,2026-06-26,2026-10-23,118,1500
airport,KJFK,2026-10-25,2026-10-25,1,6
airport,OMDB,2026-10-25,2026-10-25,1,5
airport,WSSS,2026-10-25,2026-10-25,1,3
airport,YSSY,2026-10-25,2026-10-25,1,1
airport,LFPG,2026-10-25,2026-10-25,1,9
airport,EDDF,2026-10-25,2026-10-25,1,8
airport,KLAS,2026-10-25,2026-10-25,1,1
airport,HKJK,2026-10-25,2026-10-25,1,1
airport,VGHS,2026-10-25,2026-10-25,1,1
airport,RJTT,2026-10-25,2026-10-25,1,2
airport,CYYZ,2026-10-25,2026-10-25,1,2
airport,EIDW,2026-10-25,2026-10-25,1,7
airline,BA,2026-06-25,2026-10-23,120,61840
airline,VS,2026-06-25,2026-10-23,120,6120
airline,EI,2026-06-25,2026-10-23,118,4410
airline,LH,2026-06-25,2026-10-23,120,3980
airline,AA,2026-06-25,2026-10-23,120,3310
airline,EK,2026-06-25,2026-10-23,120,1690
airline,QR,2026-06-25,2026-10-23,120,1460
type,A320,2026-06-25,2026-10-23,120,30110
type,A20N,2026-06-25,2026-10-23,120,12340
type,B789,2026-06-25,2026-10-23,120,9120
type,A35K,2026-06-25,2026-10-23,120,5230
type,B77W,2026-06-25,2026-10-23,120,4410
type,A388,2026-06-25,2026-10-23,118,1520
type,B744,2026-07-03,2026-10-11,6,6
"""


def browser():
    for c in (r'C:\Program Files\Google\Chrome\Application\chrome.exe',
              r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
              'google-chrome', 'chromium', 'chrome'):
        if Path(c).exists() or shutil.which(c):
            return c
    raise SystemExit('Chrome or Edge not found')


def shoot(url, out, w, h, scale):
    subprocess.run([browser(), '--headless=new', '--disable-gpu', '--hide-scrollbars',
                    f'--window-size={w},{h}', f'--force-device-scale-factor={scale}',
                    '--virtual-time-budget=6000', f'--screenshot={out}', url],
                   check=True, capture_output=True)
    print('wrote', out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--shots', action='store_true', help='write screenshots to docs/ and exit')
    a = ap.parse_args()
    Handler.html = page_html().replace('https://api.github.com/repos/SamHoughton/Glideslope/releases/latest',
                                       '/mock/release').replace(
        'https://api.planespotters.net/pub/photos/reg/', '/mock/photo/').encode('utf-8')   # no real photos in the docs
    Handler.frame = sample_frame()
    srv = ThreadingHTTPServer(('127.0.0.1', PORT), Handler)
    url = f'http://127.0.0.1:{PORT}/'
    if not a.shots:
        print('serving', url)
        srv.serve_forever()
        return
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    shoot(url, str(ROOT / 'docs' / 'webui-desktop.png'), 1280, 820, 1)
    shoot(url + '#alerts', str(ROOT / 'docs' / 'webui-alerts.png'), 1280, 820, 1)
    shoot(url + '#spotter', str(ROOT / 'docs' / 'webui-spotter.png'), 1280, 820, 1)
    shoot(url + 'logos', str(ROOT / 'docs' / 'webui-logos.png'), 1280, 820, 1)
    shoot(url + '#history', str(ROOT / 'docs' / 'webui-history.png'), 1280, 1100, 1)
    shoot(url + '#health', str(ROOT / 'docs' / 'webui-health.png'), 1280, 900, 1)
    shoot(url + '#stickers', str(ROOT / 'docs' / 'webui-stickers.png'), 1280, 1300, 1)
    shoot(url + '#map', str(ROOT / 'docs' / 'webui-map.png'), 1280, 900, 1)
    shoot(url + '#system', str(ROOT / 'docs' / 'webui-system.png'), 1280, 900, 1)
    today = ROOT / 'docs' / 'webui-today.png'
    shoot(url, str(today), 1280, 1240, 1)
    from PIL import Image
    Image.open(today).crop((16, 576, 624, 1032)).save(today)
    phone = ROOT / 'docs' / 'webui-phone.png'
    shoot(url + 'phone', str(phone), 600, 1100, 2)
    from PIL import Image
    Image.open(phone).crop((0, 0, 390 * 2, 1100 * 2)).save(phone)
    srv.shutdown()


if __name__ == '__main__':
    main()
