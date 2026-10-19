#!/usr/bin/env python3
"""
Build and run the firmware's unit tests (or the frame renderer) on the
desktop with the clang that ships in the `ziglang` package, for machines
without a C++ toolchain (pip install ziglang). CI uses `pio test -e native`;
this compiles the same sources with the same stubs.

    python tools/host_build.py test            # unit tests
    python tools/host_build.py render OUT_DIR  # tools/host_render.cpp -> PPM frames

Needs the native env's libraries once: python -m platformio pkg install -e native
"""
import subprocess
import sys
from pathlib import Path

FW = Path(__file__).resolve().parent.parent / "firmware"
LIBS = FW / ".pio" / "libdeps" / "native"
OUT = FW / ".pio" / "host"

LOGIC = ["config/Airport.cpp", "config/Airports.cpp", "display/ApproachModel.cpp", "display/RunwayTracker.cpp",
         "display/HoldTracker.cpp", "display/DailyStats.cpp", "utils/Weather.cpp", "adapters/ReadsbParser.cpp",
         "utils/Spotter.cpp", "utils/SeenLog.cpp"]
DRAW = ["display/FrameCanvas.cpp", "display/InfoScreens.cpp", "display/Globe.cpp", "display/Sky.cpp",
        "display/AircraftSprites.cpp", "display/OverheadScene.cpp"]
FLAGS = ["-std=gnu++17", "-O1", "-D", "NATIVE_TEST", "-D", "_POSIX_THREAD_SAFE_FUNCTIONS", "-D", "ARDUINOJSON_ENABLE_ARDUINO_STRING=1",
         "-D", "ARDUINOJSON_ENABLE_ARDUINO_STREAM=1", "-I", str(FW / "test" / "stubs"), "-I", str(FW),
         "-I", str(LIBS / "ArduinoJson" / "src"), "-w"]


def cxx(args, lang="c++", flags=FLAGS):
    cmd = [sys.executable, "-m", "ziglang", lang, *flags, *args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr[-6000:])
        sys.exit(r.returncode)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    what = sys.argv[1] if len(sys.argv) > 1 else "test"
    if what == "test":
        unity = LIBS / "Unity" / "src"
        exe = OUT / "tests.exe"
        cxx(["-c", str(unity / "unity.c"), "-I", str(unity), "-o", str(OUT / "unity.o")], "cc", ["-O1", "-w"])
        cxx([str(FW / "test" / "test_logic" / "test_main.cpp"), *(str(FW / s) for s in LOGIC),
             str(OUT / "unity.o"), "-I", str(unity), "-o", str(exe)])
        sys.exit(subprocess.run([str(exe)], cwd=FW).returncode)
    if what == "render":
        exe = OUT / "render.exe"
        srcs = [str(FW / s) for s in LOGIC + DRAW if (FW / s).exists()]
        cxx([str(FW.parent / "tools" / "host_render.cpp"), *srcs, "-o", str(exe)])
        out = Path(sys.argv[2] if len(sys.argv) > 2 else OUT / "frames")
        out.mkdir(parents=True, exist_ok=True)
        sys.exit(subprocess.run([str(exe), str(out)], cwd=FW).returncode)
    sys.exit(f"unknown target {what}")


if __name__ == "__main__":
    main()
