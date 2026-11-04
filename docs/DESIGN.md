# SegaTube design

How the player, the movie format and the converter fit together, and why.

## 1. The hardware budget

| Part | Used for |
|---|---|
| Master SH-2 | reads records, decodes audio, decodes the top part of each picture, UI, timing |
| Slave SH-2 | decodes the bottom part of each picture; its DMA channel 1 feeds the PWM |
| 68000 | the SD card (Mega EverDrive PRO I/O), the pad, the text layer |
| SDRAM (256 KiB) | code (~20 KiB), stream buffer 128 KiB, audio ring 32 KiB, tables |
| Frame buffers | two, swapped; 256 colours (320x224) or direct colour (up to 320x203) |
| Cartridge memory | the EverDrive writes file data to 0x380000 (stream) and 0x300000 (shared) |

SDRAM ends at about `0x06032000` of the `0x0603C000` allowed (`tests/verify_rom.py`).

## 2. Picture modes

- **FILM**: direct colour, BGR555 per pixel. The frame buffer holds 256 line-table
  words and 65,280 pixel words, so at most 204 rows; pictures are up to 320x200,
  the rest of the 224 lines show a black row through the line table.
- **FULL**: 256 colours, a palette per chunk (scene), 320x224.

480i (two 320x224 field pictures a frame, one frame buffer per field) was built
and dropped: on the console the 32X draws one of the two fields half a scanline
(about 210 pixels) late, so the fields never line up. The format keeps profile
number 2 and two-picture records reserved.

The encoder reserves palette entry 0 as black (letterbox and pillarbox borders).

## 3. The movie format (`src/codec/stv_format.h`)

A movie is a folder `MOVIES/<title>/`: `INFO.STI` (title, length, the modes
present, a 128x72 poster, 96x54 scrub thumbnails) and one `.STV` file per mode
(`.ST1`, `.ST2`... past 4 GB: the EverDrive's file pointer is 32-bit).

A profile file is a 4 KiB header, a chunk index, then chunks of about one second.
A chunk starts with intra pictures, so playback can start at any chunk:

- the first **two** frames are intra (frame f is decoded into the buffer that
  held frame f-2, so after a seek both buffers need a fresh picture);
- the first picture carries the chunk's palette (FULL);
- the first record carries "preroll" audio for its first quarter second.

Each record is one frame: audio (IMA ADPCM, 4 bits, stereo, 22,050 Hz, carried a
quarter second ahead of its picture so the ring never runs dry) and the
picture(s). A picture is split in two **parts** at a block row the encoder picks
so both SH-2s get the same work.

## 4. The codec (`src/codec/stv_dec.c`)

4x4 blocks, raster order, ops of up to 32 blocks:

| Op | Bytes (P8 / DC) | Meaning |
|---|---|---|
| SKIP / SKIPLONG | 0 | keep what the buffer has |
| FILL / FILLRUN | 1 / 2 | one colour |
| PAIR | 4 / 6 | two colours and a 16-bit mask |
| QUAD | 8 / 12 | four colours and 2 bits a pixel |
| RAW | 16 / 32 | every pixel |
| MOVE | 2 | copy a 4x4 from (dx, dy) in the same buffer |

Each stream (ops, colours, masks, raw, vectors) is separate and 4-byte aligned,
so the SH-2 writes whole block rows as 32-bit words. The **same decoder** runs in
the encoder on every picture it writes, into a model of the two frame buffers:
SKIP and MOVE refer to exactly what the console's buffers will hold, and the
encoder fails loudly if the decoder and its own model ever disagree.

The encoder chooses each block's op by rate-distortion (error + lambda x bytes),
raises lambda for a chunk that is over the stream rate (`--rate-cap`, default
900 KB/s), over the largest record the player buffers (48 KiB), or over the decode
time budget per SH-2.

## 5. Streaming from the SD card

The EverDrive's command `0xCB` (file read to memory) has the EverDrive's own
processor write the file into cartridge memory while the 68000 waits in work RAM
(`md_halt.s`); nothing may touch the cartridge meanwhile. So:

1. the master asks the 68000 for 32 KiB (`CMD_READ`), and keeps decoding from SDRAM;
2. when the 68000 answers, DMA channel 0 copies the 32 KiB from the cartridge into
   the 128 KiB stream buffer in SDRAM;
3. records are decoded from SDRAM.

The ROM header says `SEGA SSF` so the EverDrive turns on its extended mapper and
I/O (ZERO SECTOR's hardware-tested setup). Test builds carry their movies in the
ROM instead (`tools/mkvfs.py`), read through the same calls.

## 6. Timing

The audio ring's playback position is the clock. A frame goes on screen at the
vblank nearest its time (24 fps shows as 3:2 on NTSC, 30 fps as 2:2). A player
more than half a second late skips to the next chunk.

## 7. The COMM protocol (`src/platform/md/md.h`)

One command at a time from the master (`C_CMD`: pad ack, sequence, command;
`C_P1..3` parameters); the 68000 answers in `C_REPLY`. The 68000 also publishes the
pad (with presses latched until acknowledged; the player drops presses made
while nobody was reading, e.g. during "THE END"), a vblank counter and flags
(EverDrive present, built-in movies, 3-button pad, cartridge bus lent to the
EverDrive).

The text layer is the Mega Drive VDP in front of the 32X picture: text on plane
A, the panels behind it (time bar, the controls panel) on plane B, since the
font's background is transparent.

## 8. Testing

- `make check`: ROM checks (header, checksum, layout, marker strings), host tests
  (every mode decodes, every seek decodes, decoder fuzzing under ASan) and the
  PicoDrive scripts (`tests/scripts`): the library, both modes, pause,
  fast forward, rewind, the scrubber, the end of a movie, the controls panel
  (6-button and 3-button pads), the hardware test ROM.
- `rom/segatube-probe.32x` (`make probe`): on the console, measures the SD card
  streaming rate, decode time and sound (see the README).

Not covered by the emulator: the EverDrive's SD access (no emulator has it),
the console's real SH-2 bus timing.

## 9. Toolchain

`sh-elf` and `m68k-elf` GCC 12.4 (C only, libgcc, no newlib) in `/opt/toolchains/sega`,
built from Ubuntu's `gcc-12` source package with Ubuntu's `binutils-sh-elf` and
`binutils-m68k-linux-gnu`; `src/platform/include/string.h` and `libc_min.c` stand
in for the C library. Chilly Willy's 32XDK (20220418, GCC 12.1) should also work.
