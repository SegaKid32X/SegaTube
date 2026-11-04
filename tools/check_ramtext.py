#!/usr/bin/env python3
"""Check the 68000's work-RAM code (the EverDrive driver, md_ed.c): while it
runs, RV = 1 and the cartridge is not where the 68000 code expects it, so it
must never jump or call outside work RAM (0xFF0000-0xFFFFFF).

usage: check_ramtext.py OBJDUMP MD.ELF
"""
import re
import subprocess
import sys

objdump, elf = sys.argv[1], sys.argv[2]
out = subprocess.run([objdump, "-d", "-j", ".data", elf], capture_output=True, text=True, check=True).stdout
bad = []
code = out.split("<spin_limit>:")[0]          # (stop at the driver's data)
for line in code.splitlines():
    m = re.match(r"\s*([0-9a-f]+):\s+(?:[0-9a-f]{4} )+\s*(\S+)\s+(.*)", line)
    if not m:
        continue
    op, args = m.group(2), re.sub(r"<[^>]*>", "", m.group(3))   # (drop objdump's symbol notes)
    # any absolute address in the cartridge's 68000 window (code, constants)
    for t in re.findall(r"\b(?:0x)?(88[0-9a-f]{4}|9[0-9a-f]{5})\b", args):
        bad.append(line.strip())
    if op.startswith(("jsr", "jmp", "bsr", "bra", "b", "db")):
        for t in re.findall(r"0x([0-9a-f]{5,8})", args):
            a = int(t, 16)
            if op.startswith(("jsr", "jmp", "bsr", "bra")) and not (0xFF0000 <= a <= 0xFFFFFF):
                bad.append(line.strip())
if bad:
    print("check_ramtext: work-RAM code reaches outside work RAM:")
    for b in bad:
        print("   ", b)
    sys.exit(1)
print("check_ramtext: OK (the EverDrive driver stays in work RAM)")
