#!/usr/bin/env python3
"""Extract the Sega-designated 32X initial program (ICD_MARS, 0x3F0..0x7FF).

The 32X boot ROM compares the cartridge's 68000 code at 0x3F0 against its own
copy and locks the adapter if it differs, so every 32X cartridge must carry
these 1040 bytes unchanged (32X Hardware Manual, section 5.2 "Security").
We take them from Chilly Willy's public 32X support code instead of keeping a
hand-typed copy in this tree.

usage: extract_icd.py <crt0.s from Chilly Willy's SEGA 32X support code> <out.bin>
"""
import hashlib, re, sys

src, out = sys.argv[1], sys.argv[2]
text = open(src).read()
start = text.index("Standard 32X startup code for MD side at 0x3F0")
body = text[start:]
words = []
for line in body.splitlines()[1:]:
    line = line.strip()
    if not line.startswith(".word"):
        if words:
            break
        continue
    words += [int(w, 16) for w in re.findall(r"0x[0-9A-Fa-f]+", line)]
blob = b"".join(w.to_bytes(2, "big") for w in words)
if len(blob) != 0x410:
    sys.exit(f"expected 0x410 bytes, got {len(blob):#x}")
if b"MARS Initial & Security Program" not in blob:
    sys.exit("blob does not look like the MARS initial program")
open(out, "wb").write(blob)
print(f"{out}: {len(blob)} bytes sha1={hashlib.sha1(blob).hexdigest()}")
