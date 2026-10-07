#!/usr/bin/env python3
"""
Make an airport pack for Glideslope: everything the board needs to watch an
airport other than Heathrow, in one file to upload from the board's web page.

  python tools/airport_pack.py EGKK --tz Europe/London
  python tools/airport_pack.py KSFO --tz America/Los_Angeles --name "SAN FRAN"

Writes <ICAO>.airport (and <ICAO>-preview.png) in the current folder.

Sources (downloaded on first use, cached in tools/.cache):
  - OurAirports (public domain): airport position, IATA code and every
    runway end (threshold position, heading, displaced threshold).
  - OpenStreetMap via the Overpass API (ODbL): rivers, lakes, reservoirs,
    motorways, parks, aprons and runways for the 128x64 base map.
  - Time zone: --tz (IANA name; looked up from the position if the
    timezonefinder package is installed), turned into a POSIX TZ rule from
    the tzdata package or the posix_tz_db table.

Map: 128x64 pixels, equirectangular with square pixels, 0.2 degrees of
latitude tall (about 22 km) centred on the airport, or on --center lat,lon
(e.g. your home, so the map shows the approach past it).

Pack format: one line of JSON, a newline, then 4096 bytes: the map as 4-bit
palette indices, row-major, high nibble = left pixel. Palette (fixed, see
firmware/display/MapBase.h): 0 empty, 1 river, 2 lakes/reservoirs, 3 runways,
4 aprons, 5 motorways, 6 parks, 7 approach lanes.
"""
import argparse
import csv
import io
import json
import math
import sys
import urllib.parse
import urllib.request
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
CACHE = ROOT / '.cache'
UA = {'User-Agent': 'Glideslope airport_pack (+https://github.com/SamHoughton/Glideslope)'}

W, H = 128, 64
SS = 4                      # supersampling for the map
LAT_SPAN = 0.2              # degrees of latitude the map covers

PALETTE = [(0, 0, 0), (20, 55, 110), (14, 34, 70), (95, 100, 112),
           (24, 26, 32), (30, 30, 38), (12, 40, 18), (34, 34, 46)]
EMPTY, RIVER, LAKE, RUNWAY, APRON, MOTORWAY, PARK, LANE = range(8)


def fetch(url, cache_name=None, data=None):
    if cache_name:
        path = CACHE / cache_name
        if path.exists():
            return path.read_bytes()
    req = urllib.request.Request(url, data=data, headers=UA)
    with urllib.request.urlopen(req, timeout=180) as r:
        body = r.read()
    if cache_name:
        CACHE.mkdir(exist_ok=True)
        path.write_bytes(body)
    return body


def ourairports(name):
    raw = fetch(f'https://davidmegginson.github.io/ourairports-data/{name}', name)
    return list(csv.DictReader(io.StringIO(raw.decode('utf-8'))))


def fnum(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def bearing(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return (math.degrees(math.atan2(y, x)) + 360) % 360


def move(lat, lon, course, metres):
    c = math.radians(course)
    return (lat + metres * math.cos(c) / 111320.0,
            lon + metres * math.sin(c) / (111320.0 * math.cos(math.radians(lat))))


def short_name(full, icao):
    drop = {'AIRPORT', 'INTERNATIONAL', 'INTL', 'AERODROME', 'AIRFIELD', 'REGIONAL', 'MUNICIPAL', 'FIELD'}
    words = [w for w in full.upper().replace('-', ' ').split() if w.strip('.') not in drop]
    name = ' '.join(words)
    if len(name) > 12 and words:
        name = words[-1]          # "LONDON GATWICK" -> "GATWICK"
    return (name or icao)[:12]


def posix_tz(iana):
    try:
        import importlib.resources as res
        import tzdata  # noqa: F401
        data = res.files('tzdata').joinpath('zoneinfo', *iana.split('/')).read_bytes()
        return data.rstrip(b'\n').split(b'\n')[-1].decode()
    except Exception:
        pass
    table = json.loads(fetch('https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.json',
                             'posix_tz_db.json'))
    if iana not in table:
        sys.exit(f'Unknown time zone {iana!r} (use an IANA name such as Europe/London)')
    return table[iana]


def runway_ends(icao):
    ends = []
    for r in ourairports('runways.csv'):
        if r['airport_ident'] != icao or r['closed'] == '1':
            continue
        for a, b in (('le', 'he'), ('he', 'le')):
            ident = r[f'{a}_ident'].strip().upper()
            lat, lon = fnum(r[f'{a}_latitude_deg']), fnum(r[f'{a}_longitude_deg'])
            olat, olon = fnum(r[f'{b}_latitude_deg']), fnum(r[f'{b}_longitude_deg'])
            if not ident or ident.startswith('H') or lat is None or lon is None:
                continue
            crs = fnum(r[f'{a}_heading_degT'])
            if olat is not None and olon is not None:
                crs = bearing(lat, lon, olat, olon)   # towards the far end: the landing direction
            if crs is None:
                continue
            disp = fnum(r[f'{a}_displaced_threshold_ft']) or 0
            if disp:
                lat, lon = move(lat, lon, crs, disp * 0.3048)
            ends.append({'n': ident[:3], 'lat': round(lat, 5), 'lon': round(lon, 5), 'crs': round(crs, 1),
                         'far': (olat, olon)})
    return ends


def overpass(s, w, n, e):
    q = f'''[out:json][timeout:120];
(
  way["aeroway"="runway"]({s},{w},{n},{e});
  way["aeroway"="apron"]({s},{w},{n},{e});
  way["highway"="motorway"]({s},{w},{n},{e});
  way["waterway"="river"]({s},{w},{n},{e});
  way["natural"="water"]({s},{w},{n},{e});
  relation["natural"="water"]({s},{w},{n},{e});
  way["landuse"="reservoir"]({s},{w},{n},{e});
  way["leisure"="park"]({s},{w},{n},{e});
);
out geom;'''
    key = f'osm_{s:.3f}_{w:.3f}_{n:.3f}_{e:.3f}.json'
    body = fetch('https://overpass-api.de/api/interpreter', key,
                 data=urllib.parse.urlencode({'data': q}).encode())
    return json.loads(body)['elements']


def classify(tags):
    if tags.get('aeroway') == 'runway':
        return RUNWAY
    if tags.get('aeroway') == 'apron':
        return APRON
    if tags.get('highway') == 'motorway':
        return MOTORWAY
    if tags.get('waterway') == 'river':
        return RIVER
    if tags.get('natural') == 'water' and tags.get('water') in ('river', 'canal', 'stream'):
        return RIVER
    if tags.get('natural') == 'water' or tags.get('landuse') == 'reservoir':
        return LAKE
    if tags.get('leisure') == 'park':
        return PARK
    return None


def render_map(elements, bbox, ends):
    n, s, w, e = bbox
    layers = {k: Image.new('1', (W * SS, H * SS), 0) for k in (RIVER, LAKE, RUNWAY, APRON, MOTORWAY, PARK)}

    def xy(lat, lon):
        return ((lon - w) / (e - w) * W * SS, (n - lat) / (n - s) * H * SS)

    for el in elements:
        cls = classify(el.get('tags', {}))
        if cls is None:
            continue
        rings = []
        if el['type'] == 'way' and 'geometry' in el:
            rings.append(el['geometry'])
        elif el['type'] == 'relation':
            rings += [m['geometry'] for m in el.get('members', []) if m.get('role') == 'outer' and 'geometry' in m]
        d = ImageDraw.Draw(layers[cls])
        for g in rings:
            pts = [xy(p['lat'], p['lon']) for p in g]
            if len(pts) < 2:
                continue
            closed = len(pts) > 3 and pts[0] == pts[-1]
            if cls in (LAKE, PARK, APRON) or (cls == RIVER and closed and el['type'] != 'way') or \
               (cls == RIVER and closed and el.get('tags', {}).get('natural') == 'water'):
                d.polygon(pts, fill=1)
            else:
                width = {RUNWAY: SS + 2, MOTORWAY: SS, RIVER: SS + 1}.get(cls, SS)
                d.line(pts, fill=1, width=width)

    # Each map pixel: the highest-priority feature covering enough of it.
    need = {RUNWAY: 2, APRON: 6, MOTORWAY: 3, RIVER: 3, LAKE: 7, PARK: 8}
    order = [RUNWAY, APRON, RIVER, LAKE, MOTORWAY, PARK]
    px = {k: layers[k].load() for k in layers}
    grid = [[EMPTY] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            for cls in order:
                cov = sum(px[cls][x * SS + i, y * SS + j] for i in range(SS) for j in range(SS))
                if cov >= need[cls]:
                    if cls == MOTORWAY and (x + y) % 2:   # dotted, as on the London map
                        continue
                    grid[y][x] = cls
                    break

    # Approach lanes: dotted along each extended centreline, 2-14 km out.
    for r in ends:
        back = (r['crs'] + 180) % 360
        for k in range(2, 15):
            lat, lon = move(r['lat'], r['lon'], back, k * 1000)
            fx, fy = xy(lat, lon)
            x, y = int(fx / SS), int(fy / SS)
            if 0 <= x < W and 0 <= y < H and grid[y][x] == EMPTY and k % 2 == 0:
                grid[y][x] = LANE
    return grid


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('icao', help='ICAO airport code, e.g. EGKK')
    ap.add_argument('--tz', help='IANA time zone, e.g. Europe/London')
    ap.add_argument('--name', help='short name for the panel (max 12 characters)')
    ap.add_argument('--center', help='map centre as lat,lon (default: the airport)')
    ap.add_argument('--out', help='output folder (default: current)')
    a = ap.parse_args()
    icao = a.icao.upper()

    apt = next((r for r in ourairports('airports.csv') if r['ident'] == icao), None)
    if not apt:
        sys.exit(f'{icao} not found in OurAirports')
    lat, lon = float(apt['latitude_deg']), float(apt['longitude_deg'])
    iata = (apt.get('iata_code') or '').strip().upper()
    name = (a.name or short_name(apt['name'], icao)).upper()[:12]

    tz_name = a.tz
    if not tz_name:
        try:
            from timezonefinder import TimezoneFinder
            tz_name = TimezoneFinder().timezone_at(lat=lat, lng=lon)
        except ImportError:
            sys.exit('Give the time zone with --tz (e.g. --tz Europe/London), or pip install timezonefinder')
    tz = posix_tz(tz_name)

    ends = runway_ends(icao)
    if not ends:
        sys.exit(f'No runways for {icao} in OurAirports')

    clat, clon = (float(v) for v in a.center.split(',')) if a.center else (lat, lon)
    lon_span = LAT_SPAN * 2 / math.cos(math.radians(clat))   # 2:1 map with square pixels
    bbox = (clat + LAT_SPAN / 2, clat - LAT_SPAN / 2, clon - lon_span / 2, clon + lon_span / 2)
    print(f'{icao} {name}: {len(ends)} runway ends, tz {tz_name} -> {tz}')
    print('Fetching map data from OpenStreetMap...')
    grid = render_map(overpass(bbox[1], bbox[2], bbox[0], bbox[3]), bbox, ends)

    header = {
        'v': 1, 'icao': icao, 'iata': iata, 'name': name,
        'lat': round(lat, 5), 'lon': round(lon, 5), 'tz': tz,
        'skyline': 'london' if icao in ('EGLL', 'EGLC') else 'generic',
        'alternation': icao == 'EGLL',
        'runways': [{k: r[k] for k in ('n', 'lat', 'lon', 'crs')} for r in ends],
        'map': {'n': round(bbox[0], 5), 's': round(bbox[1], 5), 'w': round(bbox[2], 5), 'e': round(bbox[3], 5)},
    }
    packed = bytearray()
    for y in range(H):
        for x in range(0, W, 2):
            packed.append((grid[y][x] << 4) | grid[y][x + 1])
    out = Path(a.out or '.')
    pack = out / f'{icao}.airport'
    pack.write_bytes(json.dumps(header, separators=(',', ':')).encode() + b'\n' + bytes(packed))

    img = Image.new('RGB', (W, H))
    img.putdata([PALETTE[grid[y][x]] for y in range(H) for x in range(W)])
    img = img.resize((W * 6, H * 6), Image.NEAREST)
    d = ImageDraw.Draw(img)
    for r in ends:   # mark thresholds on the preview only
        x = (r['lon'] - bbox[2]) / (bbox[3] - bbox[2]) * W * 6
        y = (bbox[0] - r['lat']) / (bbox[0] - bbox[1]) * H * 6
        d.ellipse([x - 3, y - 3, x + 3, y + 3], outline=(255, 200, 60))
    img.save(out / f'{icao}-preview.png')
    print(f'Wrote {pack} ({pack.stat().st_size} bytes) and {icao}-preview.png')
    print('Install it from the board\'s web page: Airport -> Install airport.')


if __name__ == '__main__':
    main()
