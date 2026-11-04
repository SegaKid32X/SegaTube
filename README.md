# SegaTube

A movie player for the **Sega 32X** with a **Mega EverDrive PRO**: put converted
movies on the SD card, pick one from a poster library, and watch it with VHS-style
fast forward/rewind, pause and a scrubber with thumbnails.

> Status: in development. The format, encoder, converter and player are written;
> emulator and hardware testing are in progress. See "Testing" below.

## Picture modes

| Mode | What you get | Notes |
|---|---|---|
| **FILM** | 15-bit direct colour: any of 32,768 colours per pixel | up to 320x200 (fits widescreen films); the "LaserDisc" mode |
| **FULL** | 256 colours per scene, from 32,768 | the full 320x224 screen |
| **480i** | 256 colours per scene, 320x448 interlaced | every TV field shows its own lines, like VHS; *experimental* until tested on hardware |

Press **C** while watching to switch modes (the movie keeps its place).

## Controls

| Button | Library | Playing |
|---|---|---|
| D-pad | choose a movie | **Right/Left**: fast forward / rewind (press again: 2x up to 32x) |
| | | **Down/Up**: the scrubber (Left/Right move, hold to speed up; A go, B cancel) |
| A / Start | play | Start: pause; A: show the time bar |
| B | | back to the library |
| C | choose the picture mode | switch picture mode |
| X / Z (6-button) | | back 10 s / ahead 30 s |
| Mode (6-button) | | 480i: swap the field order if the picture looks jagged |

## Converting movies

The 32X cannot decode MP4 itself (two 23 MHz CPUs), so movies are converted once
on a PC or Mac:

```bash
python3 converter/segatube_convert.py "My Movie.mp4" --sd /Volumes/EVERDRIVE
```

or run `segatube_convert.py` with no arguments for a window to drop files into.
Each movie becomes `MOVIES/<title>/` with `INFO.STI` (title, poster, thumbnails)
and `FILM.STV`, `FULL.STV`, `I480.STV`. Needs ffmpeg (or `pip install imageio-ffmpeg`)
and the `stvenc` encoder (`make tools`).

## Building

```bash
make            # rom/segatube.32x: the player
make test-rom   # rom/segatube-test.32x: with a generated test movie built in
make tools      # obj/stvenc (encoder), obj/stvplay (desktop reference player)
```

Needs `sh-elf` and `m68k-elf` GCC in `/opt/toolchains/sega` (Chilly Willy's 32XDK
layout); see `docs/DESIGN.md`.

## How it works

See [docs/DESIGN.md](docs/DESIGN.md): the movie format, the codec, how the
EverDrive streams from the SD card into cartridge memory, and how the two SH-2s,
the PWM audio and the 68000 share the work.

## Credits

- The 32X start-up code, test harness and many hardware lessons come from
  ZERO SECTOR's development guide.
- The EverDrive I/O protocol follows krikzz's
  [mega-ed-pub](https://github.com/krikzz/mega-ed-pub) (MIT), and the streaming
  idea is MegaColor's.
- The library/catalog and chunked-index ideas were inspired by Joey Parrish's
  [Kinetoscope](https://github.com/joeyparrish/kinetoscope).
