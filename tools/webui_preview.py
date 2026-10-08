#!/usr/bin/env python3
"""
Serve the board's web page on this computer with made-up data, for
screenshots and for working on the page without a board.

  python tools/webui_preview.py                 # http://127.0.0.1:8765/
  python tools/webui_preview.py --shots         # docs/webui-desktop.png, docs/webui-phone.png

The page is extracted from firmware/utils/WebConfig.cpp (the C string the
board serves), so it is exactly what the board shows. The API answers with
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


def c_string(lit):
    return bytes(lit, 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')


def page_html():
    macros = {}
    for name, val in re.findall(r'#define (\w+) "(.*)"', (ROOT / 'firmware/utils/BrandMark.h').read_text(encoding='utf-8')):
        macros[name] = val
    src = (ROOT / 'firmware/utils/WebConfig.cpp').read_text(encoding='utf-8')
    body = src[src.index('const char kHtmlPage[] ='):]
    body = body[:body.index(';\n')]
    out = []
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"|\b(GS_[A-Z_]+)\b|//[^\n]*', body):
        if m.group(1) is not None:
            out.append(c_string(m.group(1)))
        elif m.group(2):
            out.append(macros[m.group(2)])
    return ''.join(out)


CONFIG = {
    'tar1090_host': '', 'center_lat': 51.47, 'center_lon': -0.4543, 'home_lat': 0, 'home_lon': 0,
    'radius_km': 20, 'min_altitude_ft': 100, 'display_brightness': 128,
    'text_color_r': 255, 'text_color_g': 255, 'text_color_b': 255,
    'display_nearest_only': False, 'display_border': True, 'show_flight_number_with_logo': True,
    'show_aircraft_type': True, 'show_aircraft_registration': False, 'swap_aircraft_type_reg': False,
    'screen_facing': 'S', 'display_flip': False, 'night_mode_enabled': True,
    'night_start_minutes': 1320, 'night_end_minutes': 420, 'night_brightness': 38, 'utc_offset_minutes': 0,
    'fetch_interval_seconds': 30, 'local_fetch_interval_seconds': 5, 'display_cycle_seconds': 10,
    'card_lead_seconds': 120, 'interlude_seconds': 15, 'screens': 15,
    'aeroapi_cache_ttl_seconds': 1800, 'aeroapi_fail_cache_ttl_seconds': 300,
    'opensky_client_id': '', 'opensky_client_secret': '', 'opensky_priority': False,
    'use_community_feeds': True, 'aeroapi_key': '',
}
STATUS = {
    'version': 'v1.3.0', 'built': 'Oct  7 2026 18:00:00', 'uptime_s': 86400, 'last_reset': 'power on',
    'heap_free': 84000, 'heap_max_block': 55000, 'heap_min_free': 18000, 'display_frames': 1700000,
    'web_requests': 5200, 'metar': 'METAR EGLL 071650Z 36011KT 9999 FEW030 12/06 Q1012 NOSIG',
    'airport': 'EGLL', 'airport_name': 'HEATHROW', 'airport_lat': 51.47, 'airport_lon': -0.4543, 'airport_pack': False,
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


def sample_frame():
    cache = ROOT / 'tools' / '.cache' / 'showcase_samples.pickle'
    if cache.exists():
        samples = pickle.loads(cache.read_bytes())
        px = min(samples, key=lambda s: abs(s[0] - 6.0))[2]
        import struct
        return struct.pack('<8192H', *px)
    return bytes(16384)


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
        elif p == '/api/config':     self.send(CONFIG)
        elif p == '/api/status':     self.send(STATUS)
        elif p == '/api/display':    self.send(DISPLAY)
        elif p == '/api/frame':      self.send(self.frame, 'application/octet-stream')
        elif p == '/api/log':        self.send({'cursor': len(LOG), 'lines': LOG if 'cursor=0' in self.path else []})
        else:                        self.send_error(404)

    def do_POST(self):
        self.send({'ok': True})


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
    Handler.html = page_html().encode('utf-8')
    Handler.frame = sample_frame()
    srv = ThreadingHTTPServer(('127.0.0.1', PORT), Handler)
    url = f'http://127.0.0.1:{PORT}/'
    if not a.shots:
        print('serving', url)
        srv.serve_forever()
        return
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    shoot(url, str(ROOT / 'docs' / 'webui-desktop.png'), 1280, 1180, 1)
    shoot(url, str(ROOT / 'docs' / 'webui-phone.png'), 390, 1500, 2)
    srv.shutdown()


if __name__ == '__main__':
    main()
