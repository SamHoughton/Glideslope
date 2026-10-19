#!/usr/bin/env python3
"""
Generate firmware/config/AirportDb.h from OurAirports public-domain data:
every airport with both an ICAO and an IATA code, with its position and the
name of the city it serves (for the city-names setting and the globe).

Usage:
    python tools/gen_airports.py                        # download fresh CSV
    python tools/gen_airports.py --local airports.csv   # use a local copy

The city is OurAirports' "municipality", tidied: brackets and anything
after a comma or slash dropped, accents folded to ASCII (the panel font has
none). Where the municipality is a suburb the airport sits in (Zaventem for
Brussels, Ingliston for Edinburgh), the airport's name usually starts with
the city, so a name prefix that is itself a place within 60 km is preferred.
OVERRIDES fixes what that misses; keep it short and to well-known airports.
"""

import argparse
import csv
import math
import re
import sys
import unicodedata
import urllib.request
from collections import defaultdict
from pathlib import Path

CSV_URL = "https://davidmegginson.github.io/ourairports-data/airports.csv"
OUT_PATH = Path(__file__).parent.parent / "firmware" / "config" / "AirportDb.h"
MAX_CITY = 15   # characters kept; longer names are cut at a word where possible

OVERRIDES = {
    "IAD": "Washington", "DCA": "Washington", "BWI": "Baltimore", "EWR": "Newark",
    "FRA": "Frankfurt", "HHN": "Frankfurt Hahn", "CGN": "Cologne", "DUS": "Dusseldorf",
    "MUC": "Munich", "NUE": "Nuremberg", "LEJ": "Leipzig", "VCE": "Venice", "NAP": "Naples",
    "FLR": "Florence", "GOA": "Genoa", "TRN": "Turin", "MXP": "Milan", "LIN": "Milan",
    "BGY": "Bergamo", "VRN": "Verona", "LYS": "Lyon", "MRS": "Marseille", "LIL": "Lille",
    "BSL": "Basel", "MLH": "Basel", "ATH": "Athens", "CFU": "Corfu", "CHQ": "Chania",
    "EFL": "Kefalonia", "BRU": "Brussels", "CRL": "Charleroi", "EDI": "Edinburgh",
    "EMA": "East Midlands", "NCL": "Newcastle", "LTN": "Luton", "SEN": "Southend",
    "LCY": "London", "STN": "London", "IOM": "Isle of Man", "KRK": "Krakow", "WMI": "Warsaw",
    "OTP": "Bucharest", "ZAG": "Zagreb", "LJU": "Ljubljana", "TIA": "Tirana", "SKP": "Skopje",
    "GRZ": "Graz", "VIE": "Vienna", "PRG": "Prague", "KUL": "Kuala Lumpur", "DPS": "Bali",
    "NGO": "Nagoya", "KIX": "Osaka", "ITM": "Osaka", "NRT": "Tokyo", "HND": "Tokyo",
    "ICN": "Seoul", "GMP": "Seoul", "PEK": "Beijing", "PKX": "Beijing", "PVG": "Shanghai",
    "SHA": "Shanghai", "CAN": "Guangzhou", "SZX": "Shenzhen", "TPE": "Taipei",
    "MFM": "Macau", "BKK": "Bangkok", "DMK": "Bangkok", "SGN": "Ho Chi Minh", "CGK": "Jakarta",
    "MNL": "Manila", "CRK": "Clark", "DEL": "Delhi", "BOM": "Mumbai", "BLR": "Bangalore",
    "MAA": "Chennai", "CCU": "Kolkata", "ISB": "Islamabad", "MRU": "Mauritius",
    "SEZ": "Seychelles", "MLE": "Male", "LLW": "Lilongwe", "VVO": "Vladivostok",
    "LED": "St Petersburg", "SVO": "Moscow", "DME": "Moscow", "VKO": "Moscow",
    "IST": "Istanbul", "SAW": "Istanbul", "ADB": "Izmir", "AYT": "Antalya", "DLM": "Dalaman",
    "BJV": "Bodrum", "ECN": "Ercan", "TLV": "Tel Aviv", "DXB": "Dubai", "DWC": "Dubai",
    "AUH": "Abu Dhabi", "SHJ": "Sharjah", "DOH": "Doha", "BAH": "Bahrain", "KWI": "Kuwait",
    "MCT": "Muscat", "RUH": "Riyadh", "JED": "Jeddah", "DMM": "Dammam", "CAI": "Cairo",
    "HRG": "Hurghada", "SSH": "Sharm el Sheikh", "RAK": "Marrakech", "CMN": "Casablanca",
    "AGP": "Malaga", "ALC": "Alicante", "PMI": "Palma", "IBZ": "Ibiza", "MAH": "Menorca",
    "FUE": "Fuerteventura", "ACE": "Lanzarote", "TFS": "Tenerife", "TFN": "Tenerife",
    "LPA": "Gran Canaria", "OVD": "Asturias", "RMU": "Murcia", "FAO": "Faro", "LIS": "Lisbon",
    "OPO": "Porto", "FNC": "Madeira", "PDL": "Azores", "SID": "Sal", "BVC": "Boa Vista",
    "MLA": "Malta", "LCA": "Larnaca", "PFO": "Paphos", "KEF": "Reykjavik", "NOC": "Knock",
    "YUL": "Montreal", "YYZ": "Toronto", "YVR": "Vancouver", "YHZ": "Halifax",
    "SJU": "San Juan", "SXM": "St Maarten", "ANU": "Antigua", "BGI": "Barbados",
    "UVF": "St Lucia", "GCM": "Grand Cayman", "MBJ": "Montego Bay", "AUA": "Aruba",
    "CUR": "Curacao", "TAB": "Tobago", "POS": "Port of Spain", "NAS": "Nassau",
    "GND": "Grenada", "SKB": "St Kitts", "EZE": "Buenos Aires", "AEP": "Buenos Aires",
    "GRU": "Sao Paulo", "CGH": "Sao Paulo", "GIG": "Rio de Janeiro", "SDU": "Rio de Janeiro",
    "SCL": "Santiago", "MVD": "Montevideo", "BOG": "Bogota", "LIM": "Lima",
    "MEX": "Mexico City", "NLU": "Mexico City", "CUN": "Cancun", "SJD": "Los Cabos",
    "JNB": "Johannesburg", "CPT": "Cape Town", "DUR": "Durban", "NBO": "Nairobi",
    "ADD": "Addis Ababa", "LOS": "Lagos", "ABV": "Abuja", "ACC": "Accra",
    "DSS": "Dakar", "SYD": "Sydney", "MEL": "Melbourne", "AVV": "Melbourne",
    "AKL": "Auckland", "SIN": "Singapore", "HKG": "Hong Kong", "HNL": "Honolulu",
    "DFW": "Dallas", "DAL": "Dallas", "IAH": "Houston", "HOU": "Houston", "ORD": "Chicago",
    "MDW": "Chicago", "SFO": "San Francisco", "OAK": "Oakland", "SJC": "San Jose",
    "LAX": "Los Angeles", "SNA": "Orange County", "JFK": "New York", "LGA": "New York",
    "BOS": "Boston", "PHL": "Philadelphia", "MIA": "Miami", "FLL": "Fort Lauderdale",
    "MCO": "Orlando", "TPA": "Tampa", "ATL": "Atlanta", "DEN": "Denver", "SEA": "Seattle",
    "LAS": "Las Vegas", "PHX": "Phoenix", "MSP": "Minneapolis", "DTW": "Detroit",
    "CLT": "Charlotte", "RDU": "Raleigh", "BNA": "Nashville", "AUS": "Austin",
    "SLC": "Salt Lake City", "PDX": "Portland", "SAN": "San Diego", "MSY": "New Orleans",
    "PIT": "Pittsburgh", "CLE": "Cleveland", "CVG": "Cincinnati", "STL": "St Louis",
    "BDL": "Hartford", "GSO": "Greensboro", "JER": "Jersey", "GCI": "Guernsey",
    "ACI": "Alderney", "KBP": "Kyiv", "IEV": "Kyiv", "SCQ": "Santiago", "MME": "Teesside",
    "HUY": "Humberside", "GOI": "Goa", "GOX": "Goa", "COK": "Kochi",
}

FOLD = str.maketrans({"ø": "o", "Ø": "O", "æ": "ae", "Æ": "AE", "ß": "ss", "ł": "l", "Ł": "L",
                      "đ": "d", "ı": "i", "þ": "th", "ð": "d", "’": "'"})


def fold(s):
    return unicodedata.normalize("NFKD", s.translate(FOLD)).encode("ascii", "ignore").decode()


def tidy(m):
    m = re.sub(r"\s*\(.*?\)", "", m).split(",")[0].split("/")[0].strip()
    m = fold(m).replace("St. ", "St ")
    return re.sub(r"\s+", " ", m)


def shorten(c):
    c = re.sub(r" Island$", "", c)
    if len(c) <= MAX_CITY:
        return c
    cut = re.split(r"[ -]", c[:MAX_CITY + 1])
    while len(cut) > 1 and (len(" ".join(cut)) > MAX_CITY or cut[-1].lower() in ("de", "del", "la", "le", "do", "da")):
        cut.pop()
    cut = " ".join(cut)
    return cut if 4 <= len(cut) <= MAX_CITY else c[:MAX_CITY]


def km(a, b):
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = math.sin((la2 - la1) / 2) ** 2 + math.cos(la1) * math.cos(la2) * math.sin((lo2 - lo1) / 2) ** 2
    return 12742 * math.asin(math.sqrt(h))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--local", metavar="FILE", help="Use a local airports.csv instead of downloading")
    args = ap.parse_args()
    if args.local:
        with open(args.local, newline="", encoding="utf-8") as f:
            rows = list(csv.DictReader(f))
    else:
        print(f"Downloading {CSV_URL} ...", file=sys.stderr)
        with urllib.request.urlopen(CSV_URL) as resp:
            rows = list(csv.DictReader(resp.read().decode("utf-8").splitlines()))

    # Where each (country, place) is, from every row that names it.
    places = defaultdict(list)
    for r in rows:
        m = tidy(r["municipality"])
        if m:
            places[(r["iso_country"], m.lower())].append((float(r["latitude_deg"]), float(r["longitude_deg"])))

    out, seen = [], set()
    for r in rows:
        icao = r["ident"].strip().upper()
        iata = r["iata_code"].strip().upper()
        if not (len(icao) == 4 and icao.isalpha() and len(iata) == 3 and iata.isalpha()) or icao in seen:
            continue
        seen.add(icao)
        pos = (float(r["latitude_deg"]), float(r["longitude_deg"]))
        city = tidy(r["municipality"])
        name = fold(r["name"])
        if iata in OVERRIDES:
            city = OVERRIDES[iata]
        elif not city or city.split()[0].lower() not in name.lower():
            words = re.split(r"[\s/-]+", name)
            for n in (3, 2, 1):
                cand = " ".join(words[:n])
                pts = places.get((r["iso_country"], cand.lower()))
                if pts and len(cand) > 2 and min(km(pos, p) for p in pts) < 60:
                    city = cand
                    break
        if not city:
            city = re.sub(r"\s+(International|Regional|Municipal|Airport|Airfield|Air Base).*$", "", name)
        out.append((icao, iata, pos, shorten(city)))

    out.sort(key=lambda x: x[0])
    pool, offs = bytearray(), {}
    for *_, c in out:
        if c not in offs:
            offs[c] = len(pool) // 2   # names start on even bytes: offsets in 2-byte units
            pool += c.encode("ascii") + b"\0" * (2 - len(c) % 2)
    assert len(pool) < 2 * 65536, len(pool)
    by_iata = sorted(range(len(out)), key=lambda i: out[i][1])

    n = len(out)
    L = [
        "// Auto-generated by tools/gen_airports.py - do not edit by hand.",
        "// Source: OurAirports (https://ourairports.com/data/) - public domain.",
        f"// {n} airports with ICAO and IATA codes, sorted by ICAO; {len(offs)} city names.",
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "namespace AirportDb",
        "{",
        "struct Entry { char icao[4]; char iata[3]; int16_t lat100, lon100; uint16_t city; };   // city: offset into kCities in 2-byte units",
        "",
        f"static constexpr uint16_t kCount = {n};",
        "",
        "static const Entry kTable[] PROGMEM = {",
    ]
    for i, (icao, iata, (la, lo), c) in enumerate(out):
        L.append("    {{'%s','%s','%s','%s'},{'%s','%s','%s'},%d,%d,%d}%s" % (
            *icao, *iata, round(la * 100), round(lo * 100), offs[c], "," if i < n - 1 else ""))
    L += ["};", "", "// Entry indices sorted by IATA code.", "static const uint16_t kByIata[] PROGMEM = {"]
    for i in range(0, n, 16):
        L.append("    " + ",".join(str(x) for x in by_iata[i:i + 16]) + ",")
    L += ["};", "", "static const char kCities[] PROGMEM ="]
    text = list(offs)
    line = ""
    for c in text:
        # One literal per name, so a name starting with a digit can't extend the \0.
        piece = '"' + c.replace('"', '\\"') + "\\0" * (2 - len(c) % 2) + '" '
        if len(line) + len(piece) > 100:
            L.append("    " + line.rstrip())
            line = ""
        line += piece
    L.append("    " + line.rstrip() + ";")
    L += ["}  // namespace AirportDb", ""]
    OUT_PATH.write_text("\n".join(L), encoding="utf-8")
    print(f"{n} airports, {len(offs)} cities, pool {len(pool)} bytes -> {OUT_PATH}", file=sys.stderr)


if __name__ == "__main__":
    main()
