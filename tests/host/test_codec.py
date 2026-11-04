#!/usr/bin/env python3
"""Host tests for the movie format: decode every test movie (as played and
after a seek to each chunk), and feed the decoder corrupted files under
AddressSanitizer: it may report errors, but must never crash or read or write
out of bounds.
"""
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OBJ = ROOT / "obj"
MOVIES = [OBJ / "testsd" / "MOVIES" / "SegaTube Test", OBJ / "testsd-long" / "MOVIES" / "Long Test"]


def sh(*cmd, ok=(0,)):
    os.environ["ASAN_OPTIONS"] = "detect_leaks=0"
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if p.returncode not in ok:
        sys.exit(f"FAIL: {' '.join(map(str, cmd))} -> {p.returncode}\n{p.stdout}{p.stderr}")
    return p


def main():
    asan = OBJ / "stvplay-asan"
    sh("cc", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
       "-std=gnu11", f"-I{ROOT}/src/codec", "-o", asan,
       ROOT / "tests/host/stvplay.c", ROOT / "src/codec/stv_dec.c")
    files = [f for m in MOVIES if m.exists() for f in sorted(m.glob("*.STV"))]
    if not files:
        sys.exit("FAIL: no test movies (make test-rom test-rom-long)")
    for f in files:
        out = sh(asan, f, "--check").stdout
        chunks = int(out.split(" chunks")[0].rsplit(" ", 1)[1])
        for c in range(chunks):
            sh(asan, f, "--seek", c, "--check")
        print(f"ok   {f.parent.name}/{f.name}: {chunks} chunks, every seek decodes")
    rng = random.Random(1234)
    with tempfile.TemporaryDirectory() as tmp:
        for f in files:
            data = bytearray(f.read_bytes())
            for trial in range(25):
                bad = bytearray(data)
                for _ in range(rng.randint(1, 40)):
                    i = rng.randrange(4096 + 1024, len(bad))     # keep the header sane
                    bad[i] = rng.randrange(256)
                p = Path(tmp) / "bad.STV"
                p.write_bytes(bad)
                # 0: decoded anyway, 1: errors reported; anything else is a crash
                sh(asan, p, "--check", ok=(0, 1))
            print(f"ok   {f.name}: 25 corrupted copies, no crash")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
