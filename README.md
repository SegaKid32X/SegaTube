# SegaTube

A movie player for the **Sega 32X** with a **Mega EverDrive PRO**: put converted
movies on the SD card, pick one from a poster library, and watch it with VHS-style
fast forward/rewind, pause and a scrubber with thumbnails.

> Status: plays from the SD card on a real console (NTSC 32X, Mega EverDrive PRO)
> and in the PicoDrive emulator: the library, both modes, sound, pause, fast
> forward/rewind and the scrubber.

## Picture modes

| Mode | What you get | Notes |
|---|---|---|
| **FILM** | 15-bit direct colour: any of 32,768 colours per pixel | up to 320x200 (fits widescreen films); the "LaserDisc" mode |
| **FULL** | 256 colours per scene, from 32,768 | the full 320x224 screen |

Press **C** while watching to switch modes (the movie keeps its place).

480i was tried and dropped: the 32X draws one of the two TV fields half a
scanline late, so the fields never line up.

## Controls

Press **Mode** (6-button pad) for a panel with all the controls, in the library
or while watching; with a 3-button pad, press **B** in the library.

| Button | Library | Playing |
|---|---|---|
| D-pad | choose a movie | **Right/Left**: fast forward / rewind (press again: 2x up to 32x) |
| | | **Down/Up**: the scrubber (Left/Right move, hold to speed up; A go, B cancel) |
| A / Start | play | Start: pause; A: show the time bar |
| B | the controls panel | back to the library |
| C | choose the picture mode | switch picture mode |
| X / Z (6-button) | | back 10 s / ahead 30 s |
| Mode (6-button) | the controls panel | the controls panel (the movie waits) |

## Converting movies

The 32X cannot decode MP4 itself (two 23 MHz CPUs), so movies are converted once
on a PC or Mac with **SegaTube Converter**: download it from the repository's
Actions tab (the latest `converter` run, artifacts) or Releases, open it, add
videos, choose the SD card and press Convert. Dropping videos onto the app works
too. (macOS: the app is not signed; right-click it and choose Open the first time.)

From a terminal, with Python 3, ffmpeg and `make tools`:

```bash
python3 converter/segatube_convert.py "My Movie.mp4" --sd /Volumes/EVERDRIVE
```

Each movie becomes `MOVIES/<title>/` with `INFO.STI` (title, poster, thumbnails)
and `FILM.STV`, `FULL.STV`. Each mode takes roughly 1-2 GB per hour of film at
the default quality; `--modes film` or `--modes full` makes only one. The modes
are encoded at the same time.

## Testing on your console

1. Copy `segatube-probe.32x` and `segatube.32x` to the SD card, and convert one
   movie onto it (in `MOVIES/`).
2. Run **segatube-probe.32x** (the hardware test). It shows: whether the EverDrive
   answered, the SD read speed (KB/s), the speed with the copy into the 32X's
   memory, whether a repeated read gives the same data, the time to decode one
   picture, and plays a 2-second tone. Photograph that screen; A runs it again.
3. Run **segatube.32x**: the library should list your movie.
4. If the EverDrive part fails, `segatube-test.32x` has a short test movie built
   in and plays without the SD card.

Keep other players' `.rbf` files (MegaColor's `mega-pro.rbf`) out of the folder
that holds `segatube.32x`: the EverDrive loads an `.rbf` next to a ROM into its
FPGA, and the screen stays blank.

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
