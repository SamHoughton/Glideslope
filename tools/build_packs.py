#!/usr/bin/env python3
"""
Build the ready-made airport packs in packs/ from packs/airports.txt, plus
packs/index.json (what the board's web page offers) and packs/README.md (a
gallery of the maps).

  python tools/build_packs.py            # every airport not yet built
  python tools/build_packs.py --all      # rebuild them all
  python tools/build_packs.py EGKK KSFO  # just these

airports.txt: one airport per line, "ICAO  IANA-time-zone  [panel name]".
Downloads are cached in tools/.cache, and Overpass is asked politely (a
pause between airports), so a full build takes a while the first time.
"""
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import airport_pack  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PACKS = ROOT / 'packs'


def read_list():
    rows = []
    for line in (PACKS / 'airports.txt').read_text(encoding='utf-8').splitlines():
        line = line.split('#')[0].strip()
        if not line:
            continue
        parts = line.split(None, 2)
        rows.append((parts[0].upper(), parts[1], parts[2].strip() if len(parts) > 2 else None))
    return rows


def write_index(entries):
    entries = sorted(entries, key=lambda e: (e['country'] != 'GB', e['country'], e['name']))
    (PACKS / 'index.json').write_text(json.dumps({'v': 1, 'packs': entries}, indent=1), encoding='utf-8')
    lines = ['# Airport packs', '',
             'Ready-made packs for Glideslope. On the board\'s web page, under **Airport**, pick one',
             'from the list (or download a `.airport` file here and choose it). Heathrow is built in.', '',
             'Rebuild or add airports with `python tools/build_packs.py` (list: `airports.txt`).', '',
             'Runways: [OurAirports](https://ourairports.com/data/) (public domain).',
             'Maps: © [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors.', '',
             '| Airport | Map |', '| --- | --- |']
    for e in entries:
        place = ', '.join(x for x in (e['city'], e['country']) if x)
        lines.append(f"| **{e['name']}** ({e['icao']}{'/' + e['iata'] if e['iata'] else ''})<br>{place}<br>"
                     f"[{e['file']}]({e['file']}) | <img src=\"previews/{e['icao']}.png\" width=\"320\"> |")
    (PACKS / 'README.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    rebuild = '--all' in sys.argv
    index_path = PACKS / 'index.json'
    known = {e['icao']: e for e in json.loads(index_path.read_text())['packs']} if index_path.exists() else {}
    (PACKS / 'previews').mkdir(parents=True, exist_ok=True)
    failed = []
    for icao, tz, name in read_list():
        if args and icao not in args:
            continue
        if not args and not rebuild and icao in known and (PACKS / f'{icao}.airport').exists():
            continue
        try:
            info = airport_pack.build(icao, tz, name, out=PACKS)
        except Exception as e:   # one bad airport shouldn't stop the batch
            print(f'  {icao} failed: {e}')
            failed.append(icao)
            continue
        prev = PACKS / f'{icao}-preview.png'
        prev.replace(PACKS / 'previews' / f'{icao}.png')
        known[icao] = info
        write_index(list(known.values()))
        time.sleep(20)  # be gentle with the Overpass servers
    write_index(list(known.values()))
    print(f'{len(known)} packs in packs/' + (f'; failed: {" ".join(failed)}' if failed else ''))


if __name__ == '__main__':
    main()
