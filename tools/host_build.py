#!/usr/bin/env python3
"""
Build and run the firmware's unit tests, or render its screens, on the
desktop. Uses $CXX/$CC when set (CI: g++/gcc), else the clang that ships in
the `ziglang` package, for machines without a C++ toolchain (pip install
ziglang). The sources and stubs are the same as `pio test -e native`.

    python tools/host_build.py test               # unit tests
    python tools/host_build.py render OUT_DIR     # tools/host_render.cpp -> PPM frames
    python tools/host_build.py golden             # render and compare with firmware/test/golden
    python tools/host_build.py golden --update    # accept the current renders as the references

The renders use a fixed clock (tools/host_time.h), so a screen only changes
when its code does. The comparison allows a few pixels of difference
(floating point differs a little between compilers): a layout change, text
running into text, a missing element, changes far more.

Needs the native env's libraries once: python -m platformio pkg install -e native
"""
import os
import subprocess
import sys
from pathlib import Path

FW = Path(__file__).resolve().parent.parent / "firmware"
TOOLS = Path(__file__).resolve().parent
LIBS = FW / ".pio" / "libdeps" / "native"
OUT = FW / ".pio" / "host"
GOLDEN = FW / "test" / "golden"
EXE = ".exe" if os.name == "nt" else ""

LOGIC = ["config/Airport.cpp", "config/Airports.cpp", "display/ApproachModel.cpp", "display/RunwayTracker.cpp",
         "display/HoldTracker.cpp", "display/DailyStats.cpp", "utils/Weather.cpp", "adapters/ReadsbParser.cpp",
         "utils/Spotter.cpp", "utils/SeenLog.cpp", "utils/HttpRequest.cpp"]
DRAW = ["display/FrameCanvas.cpp", "display/Fonts.cpp", "display/InfoScreens.cpp", "display/Globe.cpp", "display/Sky.cpp",
        "display/AircraftSprites.cpp", "display/OverheadScene.cpp", "display/LandingScene.cpp",
        "display/MapRenderer.cpp", "display/Traffic.cpp", "display/CardRenderer.cpp",
        "display/LineupScreen.cpp"]
FLAGS = ["-std=gnu++17", "-O1", "-D", "NATIVE_TEST", "-D", "_POSIX_THREAD_SAFE_FUNCTIONS", "-D", "ARDUINOJSON_ENABLE_ARDUINO_STRING=1",
         "-D", "ARDUINOJSON_ENABLE_ARDUINO_STREAM=1", "-I", str(FW / "test" / "stubs"), "-I", str(FW),
         "-I", str(LIBS / "ArduinoJson" / "src"), "-w"]

# A pixel counts as different when a channel is off by more than this; a
# screen fails with more than this many different pixels.
PIXEL_TOLERANCE = 24
MAX_DIFFERENT = 60


def compiler(lang):
    env = os.environ.get("CXX" if lang == "c++" else "CC")
    return [env] if env else [sys.executable, "-m", "ziglang", lang]


def cxx(args, lang="c++", flags=FLAGS):
    r = subprocess.run([*compiler(lang), *flags, *args], capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr[-6000:])
        sys.exit(r.returncode)


def render(out):
    exe = OUT / ("render" + EXE)
    srcs = [str(FW / s) for s in LOGIC + DRAW if (FW / s).exists()]
    cxx(["-include", str(TOOLS / "host_time.h"), str(TOOLS / "host_render.cpp"), *srcs, "-o", str(exe)])
    out.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([str(exe), str(out)], cwd=FW, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit(r.returncode)


def mock(out):
    exe = OUT / ("mock" + EXE)
    srcs = [str(FW / s) for s in ["display/FrameCanvas.cpp", "display/Fonts.cpp", "display/AircraftSprites.cpp"]]
    cxx([str(TOOLS / "host_mock.cpp"), *srcs, "-o", str(exe)])
    out.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([str(exe), str(out)], cwd=FW, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit(r.returncode)


def golden(update):
    from PIL import Image, ImageChops
    frames = OUT / "golden-frames"
    for old in frames.glob("*.ppm"):
        old.unlink()
    render(frames)
    GOLDEN.mkdir(parents=True, exist_ok=True)
    rendered = sorted(frames.glob("*.ppm"))
    if update:
        for p in rendered:
            Image.open(p).convert("RGB").save(GOLDEN / (p.stem + ".png"))
        for ref in GOLDEN.glob("*.png"):
            if not (frames / (ref.stem + ".ppm")).exists():
                ref.unlink()
        print(f"{len(rendered)} reference images written to {GOLDEN}")
        return
    failed, diffs = [], []
    for p in rendered:
        ref = GOLDEN / (p.stem + ".png")
        if not ref.exists():
            failed.append(f"{p.stem}: no reference image (run golden --update)")
            continue
        a, b = Image.open(p).convert("RGB"), Image.open(ref).convert("RGB")
        if a.size != b.size:
            failed.append(f"{p.stem}: size {a.size} vs {b.size}")
            continue
        d = ImageChops.difference(a, b)
        mask = d.convert("L").point(lambda v: 255 if v > PIXEL_TOLERANCE else 0)
        n = mask.histogram()[255]   # pixels over the tolerance
        status = "ok" if n <= MAX_DIFFERENT else "CHANGED"
        print(f"  {status:8} {p.stem} ({n} pixels differ)")
        if n > MAX_DIFFERENT:
            failed.append(f"{p.stem}: {n} pixels differ")
            diffs.append((p.stem, b, a, mask))
    for ref in GOLDEN.glob("*.png"):
        if not (frames / (ref.stem + ".ppm")).exists():
            failed.append(f"{ref.stem}: no longer rendered")
    if diffs:
        # Reference | now | differences, three times size, for the CI artifact.
        s = 3
        sheet = Image.new("RGB", (3 * 128 * s + 40, len(diffs) * (64 * s + 10)), (13, 17, 23))
        for i, (name, ref, now, mask) in enumerate(diffs):
            y = i * (64 * s + 10)
            for j, im in enumerate((ref, now, mask.convert("RGB"))):
                sheet.paste(im.resize((128 * s, 64 * s), Image.NEAREST), (j * (128 * s + 20), y))
        sheet.save(OUT / "golden-diff.png")
        print(f"differences: {OUT / 'golden-diff.png'}")
    if failed:
        print("Screens changed:\n  " + "\n  ".join(failed))
        print("If the change is intended: python tools/host_build.py golden --update")
        sys.exit(1)
    print(f"{len(rendered)} screens match their references")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    what = sys.argv[1] if len(sys.argv) > 1 else "test"
    if what == "test":
        unity = LIBS / "Unity" / "src"
        exe = OUT / ("tests" + EXE)
        cxx(["-c", str(unity / "unity.c"), "-I", str(unity), "-o", str(OUT / "unity.o")], "cc", ["-O1", "-w"])
        cxx([str(FW / "test" / "test_logic" / "test_main.cpp"), *(str(FW / s) for s in LOGIC),
             str(OUT / "unity.o"), "-I", str(unity), "-o", str(exe)])
        sys.exit(subprocess.run([str(exe)], cwd=FW).returncode)
    if what == "mock":
        mock(Path(sys.argv[2]) if len(sys.argv) > 2 else OUT / "mock")
        return
    if what == "render":
        render(Path(sys.argv[2]) if len(sys.argv) > 2 else OUT / "frames")
        return
    if what == "golden":
        golden("--update" in sys.argv)
        return
    sys.exit(f"unknown target {what}")


if __name__ == "__main__":
    main()
