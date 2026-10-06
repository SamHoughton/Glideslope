#!/usr/bin/env python3
"""
build_logos_local.py — FlightWall local airline logo builder
============================================================
Reads manually downloaded logo images from a local folder, converts them to
RGB565 binary blobs, and writes them to firmware/data/logos/{ICAO}.bin
ready for upload to the ESP32 via LittleFS.

Usage:
    pip install Pillow
    python tools/build_logos_local.py --in-dir ./my_downloaded_logos [--width W] [--height H] [--out OUT_DIR]

Naming:
    Name your source image files after the airline's ICAO code (e.g., AAL.png, DAL.jpg).
    The script ignores extensions and casing, matching the prefix to the AIRLINE_LIST.
"""

import argparse
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("ERROR: Missing dependencies. Run: pip install Pillow")
    sys.exit(1)

# ─── Configurable defaults ────────────────────────────────────────────────────
DEFAULT_WIDTH  = 32
DEFAULT_HEIGHT = 32
SCRIPT_DIR     = Path(__file__).parent
DEFAULT_IN     = SCRIPT_DIR.parent / "logos_input"
DEFAULT_OUT    = SCRIPT_DIR.parent / "firmware" / "data" / "logos"

# Transparent colour: pure magenta in RGB565
TRANSPARENT_RGB565 = 0xF81F

# ─── Airline list ─────────────────────────────────────────────────────────────
AIRLINE_LIST = [
    ("AAL", "AA", "American Airlines"),
    ("ABX", "GB", "ABX Air"),
    ("ASA", "AS", "Alaska Airlines"),
    ("DAL", "DL", "Delta Air Lines"),
    ("FDX", "FX", "FedEx Express"),
    ("FFT", "F9", "Frontier Airlines"),
    ("GTI", "GS", "Atlas Air"),
    ("HAL", "HA", "Hawaiian Airlines"),
    ("JBU", "B6", "JetBlue Airways"),
    ("SKW", "OO", "SkyWest Airlines"),
    ("SUN", "SY", "Sun Country Airlines"),
    ("SWA", "WN", "Southwest Airlines"),
    ("UAL", "UA", "United Airlines"),
    ("UPS", "5X", "UPS Airlines"),

    # ── Canada ────────────────────────────────────────────────────────────────
    ("ACA", "AC", "Air Canada"),
    ("ROU", "AC", "Air Canada Rouge"),
    ("TTS", "TS", "Air Transat"),
    ("WJA", "WS", "WestJet"),

    # ── Europe ────────────────────────────────────────────────────────────────
    ("AEA", "UX", "Air Europa"),
    ("AEE", "A3", "Aegean Airlines"),
    ("AFR", "AF", "Air France"),
    ("AMC", "KM", "Air Malta"),
    ("ANE", "YW", "Air Nostrum"),
    ("ASL", "JU", "Air Serbia"),
    ("AUA", "OS", "Austrian Airlines"),
    ("AWC", "ZT", "Titan Airways"),
    ("AZA", "AZ", "ITA Airways"),
    ("BAW", "BA", "British Airways"),
    ("BEL", "SN", "Brussels Airlines"),
    ("BTI", "BT", "Air Baltic"),
    ("CAI", "XC", "Corendon Airlines"),
    ("CFE", "CJ", "BA CityFlyer"),
    ("CFG", "DE", "Condor"),
    ("CLH", "CL", "Lufthansa CityLine"),
    ("LHX", "LH", "Lufthansa City Airlines"),
    ("DLA", "EN", "Air Dolomiti"),
    ("DLH", "LH", "Lufthansa"),
    ("DSC", "4Y", "Discover Airlines"),
    ("EDW", "WK", "Edelweiss Air"),
    ("EIN", "EI", "Aer Lingus"),
    ("TLA", "EI", "Aer Lingus Teoranta"),
    ("EJU", "EC", "easyJet Europe"),
    ("EWE", "E2", "Eurowings Europe"),
    ("EWG", "EW", "Eurowings"),
    ("EXS", "LS", "Jet2.com"),
    ("EZE", "T3", "Eastern Airways"),
    ("EZS", "DS", "easyJet Switzerland"),
    ("EZY", "U2", "easyJet"),
    ("FIN", "AY", "Finnair"),
    ("FPY", "OG", "PLAY"),
    ("IBB", "NT", "Binter Canarias"),
    ("IBE", "IB", "Iberia"),
    ("IBS", "I2", "Iberia Express"),
    ("ICE", "FI", "Icelandair"),
    ("ITY", "AZ", "ITA Airways"),
    ("JAF", "TB", "TUI fly Belgium"),
    ("KLC", "WA", "KLM Cityhopper"),
    ("KLM", "KL", "KLM Royal Dutch"),
    ("KMM", "KM", "KM Malta Airlines"),
    ("LGL", "LG", "Luxair"),
    ("LOG", "LM", "Loganair"),
    ("LOT", "LO", "LOT Polish Airlines"),
    ("MMO", "FR", "Malta Air"),
    ("NAX", "DY", "Norwegian"),
    ("NBT", "N0", "Norse Atlantic Airways"),
    ("NOZ", "DY", "Norwegian Air Shuttle"),
    ("OAL", "OA", "Olympic Air"),
    ("PGT", "PC", "Pegasus Airlines"),
    ("RUK", "FR", "Ryanair UK"),
    ("RYR", "FR", "Ryanair"),
    ("RYS", "RR", "Buzz"),
    ("SAS", "SK", "Scandinavian Airlines"),
    ("SCN", "SK", "SAS Connect"),
    ("SHT", "BA", "British Airways Shuttle"),
    ("SWR", "LX", "Swiss International"),
    ("SXS", "XQ", "SunExpress"),
    ("SZS", "SK", "Scandinavian Airlines"),
    ("TAP", "TP", "TAP Air Portugal"),
    ("TFL", "OR", "TUI fly Netherlands"),
    ("TFN", "6B", "TUIfly Nordic"),
    ("THY", "TK", "Turkish Airlines"),
    ("TOM", "BY", "TUI Airways"),
    ("TRA", "HV", "Transavia Netherlands"),
    ("TUI", "X3", "TUIfly Germany"),
    ("TVF", "TO", "Transavia France"),
    ("TVS", "QS", "Smartwings"),
    ("VIR", "VS", "Virgin Atlantic"),
    ("VLF", "V7", "Volotea"),
    ("VLG", "VY", "Vueling"),
    ("WIF", "WF", "Wideroe"),
    ("WUK", "W9", "Wizz Air UK"),
    ("WZZ", "W6", "Wizz Air"),

    # ── Europe Cargo ──────────────────────────────────────────────────────────
    ("BOX", "3S", "AeroLogic"),
    ("CGG", "C8", "Challenge Airlines"),
    ("CLX", "CV", "Cargolux"),
    ("DHK", "D0", "DHL Air UK"),
    ("EAT", "QY", "DHL European Air Transport"),
    ("FDY", "5O", "ASL Airlines Belgium"),
    ("GEC", "LH", "Lufthansa Cargo"),
    ("MNB", "MB", "MNG Airlines"),
    ("NPT", "KW", "West Atlantic"),
    ("SRR", "S6", "Star Air"),
    ("SWT", "WT", "Swiftair"),
    ("TAY", "3V", "TNT Airways"),

    # ── Europe Private Jet Operators ──────────────────────────────────────────
    ("EIX", "",   "Stellaer"),
    ("NJE", "",   "NetJets Europe"),
    ("VJT", "",   "VistaJet"),

    # ── Asia-Pacific ─────────────────────────────────────────────────────────
    ("AAR", "OZ", "Asiana Airlines"),
    ("AHY", "J2", "Azerbaijan Airlines"),
    ("AIC", "AI", "Air India"),
    ("AIQ", "QZ", "AirAsia"),
    ("AIX", "IX", "Air India Express"),
    ("ALK", "UL", "SriLankan Airlines"),
    ("ANA", "NH", "All Nippon Airways"),
    ("ANZ", "NZ", "Air New Zealand"),
    ("AXM", "D7", "AirAsia X"),
    ("BTK", "ID", "Batik Air"),
    ("CAL", "CI", "China Airlines"),
    ("CCA", "CA", "Air China"),
    ("CEB", "5J", "Cebu Pacific"),
    ("CES", "MU", "China Eastern"),
    ("CHH", "HU", "Hainan Airlines"),
    ("CPA", "CX", "Cathay Pacific"),
    ("CSN", "CZ", "China Southern"),
    ("CXA", "KN", "China United Airlines"),
    ("GAR", "GA", "Garuda Indonesia"),
    ("HVN", "VN", "Vietnam Airlines"),
    ("IAW", "BI", "Royal Brunei"),
    ("IGO", "6E", "IndiGo"),
    ("JAL", "JL", "Japan Airlines"),
    ("JJP", "GK", "Jetstar Japan"),
    ("JSA", "3K", "Jetstar Asia"),
    ("JST", "JQ", "Jetstar"),
    ("KAL", "KE", "Korean Air"),
    ("KZR", "KC", "Air Astana"),
    ("LNI", "JT", "Lion Air"),
    ("MAS", "MH", "Malaysia Airlines"),
    ("PAL", "PR", "Philippine Airlines"),
    ("PIA", "PK", "Pakistan International Airlines"),
    ("QFA", "QF", "Qantas"),
    ("SEJ", "SG", "SpiceJet"),
    ("SHQ", "SC", "Shandong Airlines"),
    ("SIA", "SQ", "Singapore Airlines"),
    ("TGW", "TR", "Scoot"),
    ("THA", "TG", "Thai Airways"),
    ("TVJ", "VZ", "Thai Vietjet"),
    ("VJC", "VJ", "Vietjet Air"),
    ("UZB", "HY", "Uzbekistan Airways"),
    ("VOZ", "VA", "Virgin Australia"),
    ("XAM", "MF", "Xiamen Airlines"),

    # ── Middle East & Africa ─────────────────────────────────────────────────
    ("ABY", "G9", "Air Arabia"),
    ("APK", "P4", "Air Peace"),
    ("DAH", "AH", "Air Algérie"),
    ("ETD", "EY", "Etihad Airways"),
    ("ETH", "ET", "Ethiopian Airlines"),
    ("FDB", "FZ", "flydubai"),
    ("GBA", "GF", "Gulf Air"),
    ("KAC", "KU", "Kuwait Airways"),
    ("KQA", "KQ", "Kenya Airways"),
    ("MAC", "3O", "Air Arabia Maroc"),
    ("MEA", "ME", "Middle East Airlines"),
    ("MSR", "MS", "EgyptAir"),
    ("OMA", "WY", "Oman Air"),
    ("QTR", "QR", "Qatar Airways"),
    ("RAM", "AT", "Royal Air Maroc"),
    ("RJA", "RJ", "Royal Jordanian"),
    ("SAA", "SA", "South African Airways"),
    ("SVA", "SV", "Saudia"),
    ("TAR", "TU", "Tunisair"),
    ("UAE", "EK", "Emirates"),

    # ── Latin America ─────────────────────────────────────────────────────────
    ("AMX", "AM", "Aeromexico"),
    ("AVA", "AV", "Avianca"),
    ("AZU", "AD", "Azul Brazilian"),
    ("BWA", "BW", "Caribbean Airlines"),
    ("GLO", "G3", "Gol Linhas Aéreas"),
    ("LAN", "LA", "LATAM Airlines"),
    ("TAM", "JJ", "LATAM Brasil"),
    ("VIV", "VB", "VivaAerobus"),
    ("VOI", "Y4", "Volaris"),

    # ── Russia / CIS ─────────────────────────────────────────────────────────
    ("AFL", "SU", "Aeroflot"),
    ("SDM", "FV", "Rossiya Airlines"),
    ("SVP", "UT", "UTair"),
    # ── Heathrow regulars added for the FlightWall Mini ─────────────────────
    ("AUR", "GR", "Aurigny"),
    ("AZE", "J2", "Azerbaijan Airlines"),
    ("BCY", "WX", "CityJet"),
    ("ELY", "LY", "El Al"),
    ("GFA", "GF", "Gulf Air"),
    ("MAU", "MK", "Air Mauritius"),
    ("ROT", "RO", "TAROM"),
]


# ─── Conversion helpers ───────────────────────────────────────────────────────

def rgb_to_rgb565(r: int, g: int, b: int) -> int:
    """Convert 8-bit RGB to 16-bit RGB565."""
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F
    return (r5 << 11) | (g6 << 5) | b5


def image_to_rgb565_blob(img: Image.Image, width: int, height: int) -> bytes:
    """
    Resize img to (width, height) using high-quality downsampling, then
    convert each pixel to RGB565. Fully-transparent pixels (alpha < 16) are
    encoded as TRANSPARENT_RGB565 (0xF81F).
    """
    img = img.convert("RGBA")
    img = img.resize((width, height), Image.LANCZOS)

    blob = bytearray()
    pixels = img.load()

    for y in range(height):
        for x in range(width):
            r, g, b, a = pixels[x, y]
            if a < 16:
                pixel16 = TRANSPARENT_RGB565
            else:
                # Blend onto black background proportionally to alpha.
                r = int(r * a / 255)
                g = int(g * a / 255)
                b = int(b * a / 255)
                pixel16 = rgb_to_rgb565(r, g, b)
                if pixel16 == TRANSPARENT_RGB565:
                    pixel16 = 0xF820  # slightly off-magenta
            blob += struct.pack('<H', pixel16)

    return bytes(blob)


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Build local RGB565 airline logo blobs from an image folder for FlightWall ESP32."
    )
    parser.add_argument("--in-dir", type=Path, default=DEFAULT_IN,
                        help=f"Directory containing source images (default: {DEFAULT_IN})")
    parser.add_argument("--width",  type=int, default=DEFAULT_WIDTH,
                        help=f"Logo width in pixels (default: {DEFAULT_WIDTH})")
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT,
                        help=f"Logo height in pixels (default: {DEFAULT_HEIGHT})")
    parser.add_argument("--out",    type=Path, default=DEFAULT_OUT,
                        help=f"Output directory (default: {DEFAULT_OUT})")
    parser.add_argument("--skip-existing", action="store_true",
                        help="Skip processing if the target .bin file already exists")
    args = parser.parse_args()

    in_dir: Path = args.in_dir
    out_dir: Path = args.out

    if not in_dir.exists():
        print(f"ERROR: Input directory '{in_dir}' does not exist.")
        print("Please create it and place your downloaded logo images inside.")
        sys.exit(1)

    out_dir.mkdir(parents=True, exist_ok=True)

    width:  int = args.width
    height: int = args.height
    expected_bytes = width * height * 2

    # Map lookups for the file scanner
    airline_map = {icao.upper(): (iata, name) for icao, iata, name in AIRLINE_LIST}

    # Find valid image files in the directory
    valid_extensions = {'.png', '.jpg', '.jpeg', '.bmp', '.webp'}
    image_files = [p for p in in_dir.iterdir() if p.suffix.lower() in valid_extensions]

    print(f"FlightWall Local Logo Builder")
    print(f"  Input dir   : {in_dir}")
    print(f"  Output dir  : {out_dir}")
    print(f"  Logo size   : {width}x{height} px ({expected_bytes} bytes each)")
    print(f"  Found files : {len(image_files)} source images")
    print()

    success = 0
    skipped = 0
    failed  = 0
    unmapped = 0

    for idx, img_path in enumerate(image_files, start=1):
        # Infer ICAO from filename (e.g. "aal.png" or "AAL_logo.png" -> "AAL")
        file_stem = img_path.stem.split('_')[0].upper()
        
        if file_stem not in airline_map:
            print(f"[{idx:3d}/{len(image_files)}] {img_path.name} — SKIPPED (No matching ICAO in script list)")
            unmapped += 1
            continue

        iata, name = airline_map[file_stem]
        out_path = out_dir / f"{file_stem}.bin"
        prefix = f"[{idx:3d}/{len(image_files)}] {file_stem} ({iata}) {name}"

        if args.skip_existing and out_path.exists():
            print(f"{prefix} — skipped (exists)")
            skipped += 1
            continue

        print(f"{prefix} — processing...", end=" ", flush=True)

        try:
            with Image.open(img_path) as img:
                blob = image_to_rgb565_blob(img, width, height)
                assert len(blob) == expected_bytes, f"blob size {len(blob)} != {expected_bytes}"
                out_path.write_bytes(blob)
                print(f"OK ({len(blob)} bytes)")
                success += 1
        except Exception as e:
            print(f"FAILED ({e})")
            failed += 1

    print()
    print("─" * 60)
    print(f"Done.  {success} OK  |  {skipped} skipped  |  {failed} failed  |  {unmapped} unmapped")

    if out_dir.exists():
        total_kb = sum(p.stat().st_size for p in out_dir.glob("*.bin")) / 1024
        print(f"Total logo data: {total_kb:.1f} KB in {out_dir}")
    
    print()
    print("Next steps:")
    print("  cd firmware")
    print("  pio run --target uploadfs")


if __name__ == "__main__":
    main()
