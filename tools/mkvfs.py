#!/usr/bin/env python3
"""Build movies into a SegaTube ROM (test builds and the emulator tests).

Appends a small directory and the movie files after the ROM image, at a
1 MiB boundary, and records where in the ROM header ("STVFS@" + a 32-bit
offset at 0x1C8). Both CPUs read it (src/platform/md/md_vfs.c,
src/platform/32x/io.c).

usage: mkvfs.py ROM_IN ROM_OUT MOVIE_DIR [MOVIE_DIR ...]
       (each MOVIE_DIR holds INFO.STI and any of FILM.STV FULL.STV I480.STV)
"""
import struct
import sys
from pathlib import Path

KINDS = ["INFO.STI", "FILM.STV", "FULL.STV", "I480.STV"]
NAME_LEN = 48
MAX_ROM = 4 * 1024 * 1024


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    rom = bytearray(Path(sys.argv[1]).read_bytes())
    movies = [Path(p) for p in sys.argv[3:]]
    vfs_at = (len(rom) + 0xFFFFF) & ~0xFFFFF
    rom += b"\0" * (vfs_at - len(rom))
    head = bytearray(b"STVFS1\0\0" + struct.pack(">II", len(movies), 0))
    body = bytearray()
    table_len = 16 + 80 * len(movies)
    for m in movies:
        name = m.name.encode("ascii", "replace")[:NAME_LEN - 1].ljust(NAME_LEN, b"\0")
        offs, sizes = [], []
        for k in KINDS:
            f = m / k
            if f.exists():
                data = f.read_bytes()
                while (table_len + len(body)) % 16:
                    body += b"\0"
                offs.append(table_len + len(body))
                sizes.append(len(data))
                body += data
            else:
                offs.append(0)
                sizes.append(0)
        head += name + struct.pack(">4I", *offs) + struct.pack(">4I", *sizes)
    rom += head + body
    if len(rom) > MAX_ROM:
        print(f"mkvfs: ROM would be {len(rom)} bytes, over 4 MiB: use shorter test movies",
              file=sys.stderr)
        return 1
    rom[0x1C8:0x1CE] = b"STVFS@"
    rom[0x1CE:0x1D2] = struct.pack(">I", vfs_at)
    Path(sys.argv[2]).write_bytes(rom)
    print(f"mkvfs: {len(movies)} movie(s) at {vfs_at:#x}, ROM {len(rom):,} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
