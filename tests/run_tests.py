#!/usr/bin/env python3
"""Emulator tests: boot the test ROMs in PicoDrive and drive them.

Each tests/scripts/*.txt is run by tests/harness (a small libretro host). A
line "# rom: long" picks rom/segatube-long.32x, else rom/segatube-test.32x.
Every screenshot must be lit and colourful (the black-screen guard), the
screenshots of a script must not all be the same, and the harness must exit 0
(its expect/wait commands fail it otherwise).

    python3 tests/run_tests.py            every script
    python3 tests/run_tests.py 02         scripts whose name contains "02"
"""
from __future__ import annotations

import os
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORE = os.path.join(HERE, "emu", "picodrive_libretro.so")
HARNESS = os.path.join(HERE, "harness")
SCRIPTS = os.path.join(HERE, "scripts")
OUT = os.path.join(HERE, "out")
ROMS = {"test": "rom/segatube-test.32x", "long": "rom/segatube-long.32x"}

MIN_LIT = 0.08          # at least 8% of pixels not black
MIN_COLOURS = 6


def read_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def check_shot(path):
    w, h, px = read_ppm(path)
    n = w * h
    lit = sum(1 for i in range(0, len(px), 3) if px[i] > 8 or px[i + 1] > 8 or px[i + 2] > 8)
    colours = len({px[i:i + 3] for i in range(0, len(px), 3 * 7)})
    problems = []
    if lit < n * MIN_LIT:
        problems.append(f"dark ({lit * 100 // n}% lit)")
    if colours < MIN_COLOURS:
        problems.append(f"{colours} colours")
    return problems, zlib.crc32(px)


def run(script):
    name = os.path.splitext(os.path.basename(script))[0]
    rom = ROMS["test"]
    for line in open(script):
        if line.startswith("# rom:"):
            rom = ROMS[line.split(":", 1)[1].strip()]
    out = os.path.join(OUT, name)
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith(".ppm"):
            os.remove(os.path.join(out, f))
    p = subprocess.run([HARNESS, CORE, os.path.join(ROOT, rom), script, out],
                       capture_output=True, text=True)
    failures = []
    if p.returncode:
        failures.append(f"harness exit {p.returncode}: {p.stderr.strip()[-300:]}")
    shots = sorted(f for f in os.listdir(out) if f.endswith(".ppm"))
    crcs = set()
    for s in shots:
        problems, crc = check_shot(os.path.join(out, s))
        crcs.add(crc)
        if problems and not s.startswith("allow_dark"):
            failures.append(f"{s}: {', '.join(problems)}")
    if len(shots) > 1 and len(crcs) == 1:
        failures.append("every screenshot is the same")
    print(f"{'FAIL' if failures else 'ok  '} {name} ({len(shots)} shots)")
    for f in failures:
        print("     " + f)
    return not failures


def main():
    if not os.path.exists(CORE):
        print("SKIP: no tests/emu/picodrive_libretro.so")
        return 0
    want = sys.argv[1] if len(sys.argv) > 1 else ""
    scripts = sorted(os.path.join(SCRIPTS, f) for f in os.listdir(SCRIPTS)
                     if f.endswith(".txt") and want in f)
    ok = all([run(s) for s in scripts])
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
