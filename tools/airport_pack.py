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
import urllib.error
import urllib.request
import time
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


NAME_MAX = 16


def short_name(full, town, icao):
    """'London Gatwick Airport' (London) -> GATWICK; 'Amsterdam Airport
    Schiphol' -> SCHIPHOL; 'San Francisco International' -> SAN FRANCISCO."""
    drop = {'AIRPORT', 'INTERNATIONAL', 'INTL', 'AERODROME', 'AIRFIELD', 'REGIONAL', 'MUNICIPAL', 'FIELD'}
    words = [w for w in full.upper().replace('-', ' ').replace('/', ' ').split() if w.strip('.') not in drop]
    tw = town.upper().replace('-', ' ').split()
    if tw and words[:len(tw)] == tw and len(words) > len(tw):
        words = words[len(tw):]           # the town is already implied
    name = ' '.join(w.strip('.') for w in words)
    if len(name) > NAME_MAX and words:
        name = words[-1]
    return (name or town.upper() or icao)[:NAME_MAX]


# Zones whose official POSIX rule uses negative daylight saving (winter as
# the "DST" period), which embedded C libraries don't all handle: the
# equivalent conventional rule instead.
POSIX_OVERRIDES = {'Europe/Dublin': 'GMT0IST,M3.5.0/1,M10.5.0'}


def posix_tz(iana):
    if iana in POSIX_OVERRIDES:
        return POSIX_OVERRIDES[iana]
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
  way["natural"="coastline"]({s},{w},{n},{e});
);
out geom;'''
    key = f'osm2_{s:.3f}_{w:.3f}_{n:.3f}_{e:.3f}.json'
    overpass.cached = (CACHE / key).exists()
    data = urllib.parse.urlencode({'data': q}).encode()
    servers = ['https://overpass-api.de/api/interpreter', 'https://overpass.kumi.systems/api/interpreter']
    for attempt in range(6):
        try:
            return json.loads(fetch(servers[attempt % 2], key, data=data))['elements']
        except (urllib.error.URLError, TimeoutError) as e:
            code = getattr(e, 'code', None)
            if code is not None and code not in (429, 500, 502, 503, 504):
                raise
            wait = 20 * (attempt // 2 + 1)
            print(f'  Overpass busy ({code or "no answer"}), retrying in {wait} s...')
            time.sleep(wait)
    raise ValueError('Overpass is not answering; try again later')


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


def km_xy(lat, lon, lat0):
    return lon * 111.32 * math.cos(math.radians(lat0)), lat * 110.574


def ring_area_km2(g):
    """Shoelace area of a lat/lon ring, in km^2."""
    if len(g) < 3:
        return 0.0
    lat0 = g[0]['lat']
    pts = [km_xy(p['lat'], p['lon'], lat0) for p in g]
    return abs(sum(x1 * y2 - x2 * y1 for (x1, y1), (x2, y2) in zip(pts, pts[1:] + pts[:1]))) / 2


def line_km(g):
    lat0 = g[0]['lat'] if g else 0
    pts = [km_xy(p['lat'], p['lon'], lat0) for p in g]
    return sum(math.dist(a, b) for a, b in zip(pts, pts[1:]))


# Only what reads at 128x64 (about 350 m a pixel): the main river or two,
# sizeable lakes and reservoirs, big parks.
MIN_LAKE_KM2 = 0.15
MIN_PARK_KM2 = 0.5
MIN_RIVER_AREA_KM2 = 0.05
MAX_RIVERS = 2
MIN_CANAL_KM2 = 0.4          # the Netherlands has thousands of canals and ditches


def join_rings(parts):
    """Join way pieces (lists of points) end to end into closed rings."""
    key = lambda p: (round(p['lat'], 7), round(p['lon'], 7))
    parts = [list(g) for g in parts if len(g) >= 2]
    rings = []
    while parts:
        ring = parts.pop()
        changed = True
        while key(ring[0]) != key(ring[-1]) and changed:
            changed = False
            for i, g in enumerate(parts):
                if key(g[0]) == key(ring[-1]):   ring += g[1:]
                elif key(g[-1]) == key(ring[-1]): ring += g[-2::-1]
                elif key(g[-1]) == key(ring[0]):  ring = g[:-1] + ring
                elif key(g[0]) == key(ring[0]):   ring = g[::-1][:-1] + ring
                else:
                    continue
                parts.pop(i)
                changed = True
                break
        if key(ring[0]) == key(ring[-1]) and len(ring) > 3:
            rings.append(ring)
    return rings


def sea_layer(elements, xy, size):
    """The sea, from coastline ways (OSM keeps the water on their right-hand
    side). Each point takes the side of its nearest stretch of coastline:
    no flood fill, so a gap in the data cannot pour the sea onto the land
    and there are no seams where fills meet. Worked out on a coarser grid
    (2 samples per map pixel) and scaled up to the layer."""
    segs = []
    for el in elements:
        if el.get('tags', {}).get('natural') != 'coastline' or 'geometry' not in el:
            continue
        pts = [xy(p['lat'], p['lon']) for p in el['geometry']]
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            if (x1, y1) != (x2, y2):
                segs.append((x1, y1, x2, y2))
    if not segs:
        return None

    lw, lh = size
    gw, gh = W * 2, H * 2                 # sample grid
    sx, sy = lw / gw, lh / gh             # layer px per sample
    cell = 16                             # bucket size, in samples
    cols, rows = (gw + cell - 1) // cell, (gh + cell - 1) // cell
    buckets = {}
    for i, (x1, y1, x2, y2) in enumerate(segs):
        c0 = max(0, int(min(x1, x2) / sx) // cell); c1 = min(cols - 1, int(max(x1, x2) / sx) // cell)
        r0 = max(0, int(min(y1, y2) / sy) // cell); r1 = min(rows - 1, int(max(y1, y2) / sy) // cell)
        if c1 < 0 or r1 < 0 or c0 >= cols or r0 >= rows:
            # off the map: still matters for points near the edge
            c0, c1 = max(0, min(c0, cols - 1)), max(0, min(c1, cols - 1))
            r0, r1 = max(0, min(r0, rows - 1)), max(0, min(r1, rows - 1))
        for c in range(c0, c1 + 1):
            for r in range(r0, r1 + 1):
                buckets.setdefault((c, r), []).append(i)

    def side(px, py):
        # Search rings of buckets outwards until nothing nearer can exist.
        bc, br = int(px / sx) // cell, int(py / sy) // cell
        best, votes = 1e18, 0.0
        for ring in range(0, max(cols, rows) + 1):
            found = False
            for c in range(bc - ring, bc + ring + 1):
                for r in range(br - ring, br + ring + 1):
                    if max(abs(c - bc), abs(r - br)) != ring:
                        continue
                    for i in buckets.get((c, r), ()):
                        x1, y1, x2, y2 = segs[i]
                        dx, dy = x2 - x1, y2 - y1
                        L2 = dx * dx + dy * dy
                        t = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / L2))
                        qx, qy = x1 + t * dx - px, y1 + t * dy - py
                        d = qx * qx + qy * qy
                        cross = (dx * (py - y1) - dy * (px - x1)) / math.sqrt(L2)   # > 0: right of travel
                        if d < best - 1e-6:
                            best, votes, found = d, cross, True
                        elif abs(d - best) <= 1e-6:
                            votes += cross     # shared corner: the segments vote
            reach = (ring * cell) * min(sx, sy)
            if best < 1e18 and reach * reach > best:
                break
        return votes > 0

    grid = Image.new('1', (gw, gh), 0)
    gp = grid.load()
    for y in range(gh):
        for x in range(gw):
            if side((x + 0.5) * sx, (y + 0.5) * sy):
                gp[x, y] = 1
    return grid.resize(size, Image.NEAREST)


def render_map(elements, bbox, ends):
    n, s, w, e = bbox
    layers = {k: Image.new('1', (W * SS, H * SS), 0) for k in (RIVER, LAKE, RUNWAY, APRON, MOTORWAY, PARK)}

    def xy(lat, lon):
        return ((lon - w) / (e - w) * W * SS, (n - lat) / (n - s) * H * SS)

    # Rivers drawn as lines: keep the longest named ones only.
    river_len = {}
    for el in elements:
        t = el.get('tags', {})
        if el['type'] == 'way' and t.get('waterway') == 'river' and 'geometry' in el:
            river_len[t.get('name', '')] = river_len.get(t.get('name', ''), 0) + line_km(el['geometry'])
    river_len.pop('', None)
    main_rivers = {k for k, _ in sorted(river_len.items(), key=lambda kv: -kv[1])[:MAX_RIVERS]
                   if river_len[k] >= 0.4 * max(river_len.values())} if river_len else set()

    for el in elements:
        tags = el.get('tags', {})
        cls = classify(tags)
        if cls is None:
            continue
        rings = []
        if el['type'] == 'way' and 'geometry' in el:
            rings.append(el['geometry'])
        elif el['type'] == 'relation':
            rings += join_rings([m['geometry'] for m in el.get('members', [])
                                 if m.get('role') == 'outer' and 'geometry' in m])
        is_line = cls in (RUNWAY, MOTORWAY) or (cls == RIVER and tags.get('waterway') == 'river')
        if cls == RIVER and is_line and tags.get('name') not in main_rivers:
            continue
        d = ImageDraw.Draw(layers[cls])
        for g in rings:
            pts = [xy(p['lat'], p['lon']) for p in g]
            if len(pts) < 2:
                continue
            if is_line:
                width = {RUNWAY: SS + 2, MOTORWAY: SS, RIVER: SS}[cls]
                d.line(pts, fill=1, width=width)
                continue
            if len(pts) < 4 or pts[0] != pts[-1]:
                continue                  # not a closed outline: nothing to fill
            area = ring_area_km2(g)
            canal = tags.get('water') in ('canal', 'ditch', 'drain', 'stream')
            if (cls == LAKE and area < MIN_LAKE_KM2) or (cls == PARK and area < MIN_PARK_KM2) or \
               (cls == RIVER and area < (MIN_CANAL_KM2 if canal else MIN_RIVER_AREA_KM2)):
                continue
            d.polygon(pts, fill=1)

    sea = sea_layer(elements, xy, (W * SS, H * SS))
    if sea is not None:
        layers[LAKE] = Image.composite(Image.new('1', sea.size, 1), layers[LAKE], sea)

    # Each map pixel: the highest-priority feature covering enough of it.
    need = {RUNWAY: 2, APRON: 6, MOTORWAY: 3, RIVER: 4, LAKE: 7, PARK: 8}
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


def firmware_map_header(grid, bbox):
    """Heathrow's built-in map (firmware/display/MapBase.h): the same base
    layer as a pack, one palette digit per pixel, in flash."""
    n, s_, w, e = bbox
    rows = ',\n'.join('    "' + ''.join(str(v) for v in row) + '"' for row in grid)
    pal = ',\n'.join(f'    {{{r:3d}, {g:3d}, {b:3d}}}' for r, g, b in PALETTE)
    return ('// Generated by tools/airport_pack.py EGLL --firmware-map: do not edit by hand.\n'
            '// Heathrow\'s built-in map base layer: 128x64, one palette digit per pixel, in flash.\n'
            '#pragma once\n#include <stdint.h>\n\nnamespace MapBase\n{\n'
            '    // Bounding box (equirectangular): x = (lon - LON_W) / (LON_E - LON_W) * 128,\n'
            '    // y = (LAT_N - lat) / (LAT_N - LAT_S) * 64.\n'
            f'    constexpr double LAT_N = {n:.5f}, LAT_S = {s_:.5f};\n'
            f'    constexpr double LON_W = {w:.5f}, LON_E = {e:.5f};\n\n'
            '    // 0 empty, 1 rivers, 2 lakes and reservoirs, 3 runways, 4 aprons,\n'
            '    // 5 motorways, 6 parks, 7 approach lanes\n'
            f'    static const uint8_t kPalette[][3] = {{\n{pal}\n    }};\n\n'
            f'    static const char *const kRows[64] = {{\n{rows}\n    }};\n}}\n')


def build(icao, tz_name=None, name=None, center=None, out='.', preview=True, firmware_map=False):
    """Make <ICAO>.airport (and a preview PNG) in out; returns a summary for
    the pack index. Raises ValueError for an airport it cannot make."""
    icao = icao.upper()
    apt = next((r for r in ourairports('airports.csv') if r['ident'] == icao), None)
    if not apt:
        raise ValueError(f'{icao} not found in OurAirports')
    lat, lon = float(apt['latitude_deg']), float(apt['longitude_deg'])
    iata = (apt.get('iata_code') or '').strip().upper()
    name = (name or short_name(apt['name'], apt.get('municipality', ''), icao)).upper()[:NAME_MAX]

    if not tz_name:
        try:
            from timezonefinder import TimezoneFinder
            tz_name = TimezoneFinder().timezone_at(lat=lat, lng=lon)
        except ImportError:
            raise ValueError('give the time zone with --tz (e.g. --tz Europe/London), or pip install timezonefinder')
    tz = posix_tz(tz_name)

    ends = runway_ends(icao)
    if not ends:
        raise ValueError(f'no runways for {icao} in OurAirports')

    clat, clon = center if center else (lat, lon)
    lon_span = LAT_SPAN * 2 / math.cos(math.radians(clat))   # 2:1 map with square pixels
    bbox = (clat + LAT_SPAN / 2, clat - LAT_SPAN / 2, clon - lon_span / 2, clon + lon_span / 2)
    print(f'{icao} {name}: {len(ends)} runway ends, tz {tz_name} -> {tz}')
    grid = render_map(overpass(bbox[1], bbox[2], bbox[0], bbox[3]), bbox, ends)
    if firmware_map:
        target = Path(__file__).resolve().parent.parent / 'firmware' / 'display' / 'MapBase.h'
        target.write_text(firmware_map_header(grid, bbox), encoding='utf-8')
        print('wrote', target)

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
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    pack = out / f'{icao}.airport'
    pack.write_bytes(json.dumps(header, separators=(',', ':')).encode() + b'\n' + bytes(packed))

    if preview:
        img = Image.new('RGB', (W, H))
        img.putdata([PALETTE[grid[y][x]] for y in range(H) for x in range(W)])
        img = img.resize((W * 6, H * 6), Image.NEAREST)
        d = ImageDraw.Draw(img)
        for r in ends:   # mark thresholds on the preview only
            x = (r['lon'] - bbox[2]) / (bbox[3] - bbox[2]) * W * 6
            y = (bbox[0] - r['lat']) / (bbox[0] - bbox[1]) * H * 6
            d.ellipse([x - 3, y - 3, x + 3, y + 3], outline=(255, 200, 60))
        img.save(out / f'{icao}-preview.png')
    return {'icao': icao, 'iata': iata, 'name': name, 'city': apt.get('municipality', ''),
            'country': apt.get('iso_country', ''), 'runways': len(ends) // 2 or 1,
            'file': pack.name, 'bytes': pack.stat().st_size, 'cached': getattr(overpass, 'cached', False)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('icao', help='ICAO airport code, e.g. EGKK')
    ap.add_argument('--tz', help='IANA time zone, e.g. Europe/London')
    ap.add_argument('--name', help='short name for the panel (max 16 characters)')
    ap.add_argument('--center', help='map centre as lat,lon (default: the airport)')
    ap.add_argument('--out', help='output folder (default: current)')
    ap.add_argument('--firmware-map', action='store_true',
                    help="also write the map as Heathrow's built-in one (firmware/display/MapBase.h); "
                         'use with EGLL --center 51.475,-0.369')
    a = ap.parse_args()
    center = tuple(float(v) for v in a.center.split(',')) if a.center else None
    try:
        info = build(a.icao, a.tz, a.name, center, a.out or '.', firmware_map=a.firmware_map)
    except ValueError as e:
        sys.exit(str(e))
    print(f"Wrote {info['file']} ({info['bytes']} bytes) and {info['icao']}-preview.png")
    print("Install it from the board's web page: Airport -> Install airport.")


if __name__ == '__main__':
    main()
