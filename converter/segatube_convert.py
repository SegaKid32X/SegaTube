#!/usr/bin/env python3
"""SegaTube Converter: turns ordinary video files into SegaTube movies.

Drop MP4s (or anything ffmpeg reads) on the app, or add them in its window,
and point it at the EverDrive's SD card. Each movie becomes a folder in MOVIES/ holding INFO.STI (title, poster,
scrub thumbnails) and one file per picture mode:

    FILM.STV  15-bit colour (32,768 colours on screen), up to 320x200
    FULL.STV  256 colours chosen per scene, full 320x224
    I480.STV  256 colours, 320x448 interlaced (480i)

Usage:
    segatube_convert.py                         the window (drag files in)
    segatube_convert.py movie.mp4 ... --sd E:\\   convert onto an SD card
    segatube_convert.py movie.mp4 --out folder   convert into a folder

Options: --modes film,full,i480   --quality 1-10   --rate-cap BYTES_PER_S
         --title TEXT (one input)  --start S --duration S (a part of the video)

Needs ffmpeg (bundled in the release builds; otherwise on the PATH or from the
imageio-ffmpeg package) and the stvenc encoder next to this script.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path

HERE = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))

PROFILES = {"film": 0, "full": 1, "i480": 2}
FILE_NAMES = {"film": "FILM", "full": "FULL", "i480": "I480"}

# Mega Drive H40 pixels on a 4:3 screen: 320 across, 224 lines tall
PAR = (4 / 3) / (320 / 224)          # displayed width / height of one pixel

AUDIO_RATE = 22050
THUMB_W, THUMB_H = 96, 54
POSTER_W, POSTER_H = 128, 72
THUMB_EVERY_S = 10
INFO_HEADER = 4096


# ---------------------------------------------------------------- tools

def find_ffmpeg() -> str:
    for cand in (HERE / "ffmpeg.exe", HERE / "ffmpeg"):
        if cand.exists():
            return str(cand)
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    try:
        import imageio_ffmpeg  # type: ignore
        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        pass
    raise SystemExit("ffmpeg not found: install it, or `pip install imageio-ffmpeg`")


def find_stvenc() -> str:
    names = ["stvenc.exe", "stvenc"]
    for d in (HERE, HERE.parent / "obj", HERE / "bin"):
        for n in names:
            if (d / n).exists():
                return str(d / n)
    exe = shutil.which("stvenc")
    if exe:
        return exe
    raise SystemExit("stvenc not found: build it with `make stvenc` (or use a release build)")


# ---------------------------------------------------------------- probing

@dataclass
class Source:
    path: Path
    width: int
    height: int
    dar: float              # display aspect ratio
    fps: Fraction
    duration: float         # seconds
    has_audio: bool


def probe(ffmpeg: str, path: Path) -> Source:
    """Read size, aspect, frame rate and duration from ffmpeg's banner."""
    p = subprocess.run([ffmpeg, "-hide_banner", "-i", str(path)],
                       capture_output=True, text=True, errors="replace")
    text = p.stderr
    m = re.search(r"Duration: (\d+):(\d+):(\d+(?:\.\d+)?)", text)
    if not m:
        raise SystemExit(f"{path}: not a video ffmpeg can read")
    duration = int(m[1]) * 3600 + int(m[2]) * 60 + float(m[3])
    v = re.search(r"Stream #.*?Video: .*", text)
    if not v:
        raise SystemExit(f"{path}: no video stream")
    line = v[0]
    sz = re.search(r", (\d{2,5})x(\d{2,5})", line)
    w, h = int(sz[1]), int(sz[2])
    dar = w / h
    d = re.search(r"DAR (\d+):(\d+)", line)
    if d and int(d[2]):
        dar = int(d[1]) / int(d[2])
    rot = re.search(r"rotate\s*:\s*(-?\d+)", text) or re.search(r"rotation of (-?\d+)", text)
    if rot and abs(int(float(rot[1]))) in (90, 270):
        w, h, dar = h, w, 1 / dar
    fr = re.search(r"([\d.]+) fps", line) or re.search(r"([\d.]+) tbr", line)
    fps = Fraction(fr[1]).limit_denominator(1001) if fr else Fraction(30000, 1001)
    for exact in (Fraction(24000, 1001), Fraction(30000, 1001), Fraction(60000, 1001)):
        if abs(float(fps) - float(exact)) < 0.01:
            fps = exact
    has_audio = re.search(r"Stream #.*?Audio:", text) is not None
    return Source(path, w, h, dar, fps, duration, has_audio)


# ---------------------------------------------------------------- geometry

@dataclass
class Geometry:
    w: int          # picture width (pixels)
    h: int          # picture height: rows (FILM/FULL) or frame lines (I480)
    x: int          # screen position
    y: int
    fps: Fraction
    temporal: bool  # I480: fields from successive source frames


def fit(dar: float, max_w: int, max_h: int, par: float, h_step: int) -> tuple[int, int]:
    """Largest box with the source's shape: width a multiple of 8, height of h_step."""
    h = max_w * par / dar
    if h <= max_h:
        w = max_w
        h = int(round(h / h_step)) * h_step
    else:
        h = max_h
        w = int(round(max_h * dar / par / 8)) * 8
    return max(8, min(w, max_w)), max(h_step, min(h, max_h))


def geometry(mode: str, src: Source) -> Geometry:
    fps = min(src.fps, Fraction(30000, 1001))
    if mode == "film":
        w, h = fit(src.dar, 320, 200, PAR, 4)
        return Geometry(w, h, (320 - w) // 2, (224 - h) // 2, fps, False)
    if mode == "full":
        w, h = fit(src.dar, 320, 224, PAR, 4)
        return Geometry(w, h, (320 - w) // 2, (224 - h) // 2, fps, False)
    # i480: 448 lines in the height of 224; each field is half of them
    w, h = fit(src.dar, 320, 448, PAR * 2, 8)
    temporal = src.fps > 45
    fps = src.fps / 2 if temporal else fps
    if temporal and fps > Fraction(30000, 1001):
        fps = Fraction(30000, 1001)
    return Geometry(w, h, (320 - w) // 2, (224 - h // 2) // 2, fps, temporal)


# ---------------------------------------------------------------- INFO.STI

def to555_be(rgb: bytes) -> bytes:
    out = bytearray()
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i] * 31 // 255, rgb[i + 1] * 31 // 255, rgb[i + 2] * 31 // 255
        out += struct.pack(">H", b << 10 | g << 5 | r)
    return bytes(out)


def grab_frames(ffmpeg: str, src: Source, every: float, w: int, h: int,
                start: float, duration: float) -> list[bytes]:
    """One letterboxed w x h RGB picture every `every` seconds."""
    vf = (f"select='isnan(prev_selected_t)+gte(t-prev_selected_t\\,{every})',"
          f"scale={w}:{h}:force_original_aspect_ratio=decrease:flags=area,"
          f"pad={w}:{h}:(ow-iw)/2:(oh-ih)/2,format=rgb24")
    cmd = [ffmpeg, "-v", "error", "-ss", str(start), "-t", str(duration), "-i", str(src.path),
           "-vf", vf, "-fps_mode", "passthrough", "-f", "rawvideo", "-"]
    data = subprocess.run(cmd, capture_output=True, check=True).stdout
    n = w * h * 3
    return [data[i:i + n] for i in range(0, len(data) - n + 1, n)]


def write_info(path: Path, title: str, modes: list[str], duration: float,
               thumbs: list[bytes], every: int, poster: bytes) -> None:
    profiles = 0
    for m in modes:
        profiles |= 1 << PROFILES[m]
    thumb_off = INFO_HEADER
    poster_off = thumb_off + len(thumbs) * THUMB_W * THUMB_H * 2
    hdr = struct.pack(">IHHIIIII", 0x53544931, 1, profiles, int(duration * 1000),
                      len(thumbs), every * 1000, thumb_off, poster_off)
    hdr += b"\0" * 36
    hdr += title.encode("ascii", "replace")[:63].ljust(64, b"\0")
    hdr = hdr.ljust(INFO_HEADER, b"\0")
    with open(path, "wb") as f:
        f.write(hdr)
        for t in thumbs:
            f.write(to555_be(t))
        f.write(to555_be(poster))


# ---------------------------------------------------------------- converting

def safe_name(title: str) -> str:
    s = re.sub(r"[^A-Za-z0-9 _\-]+", "", title).strip()
    s = re.sub(r"\s+", " ", s)
    return (s or "MOVIE")[:40]


def ascii_title(title: str) -> str:
    return "".join(c if 32 <= ord(c) < 127 else "?" for c in title)[:63]


def convert(src_path: Path, dest_root: Path, modes: list[str], quality: float,
            rate_cap: int, title: str | None = None, start: float = 0.0,
            duration: float | None = None, log=print, progress=None) -> Path:
    ffmpeg, stvenc = find_ffmpeg(), find_stvenc()
    src = probe(ffmpeg, src_path)
    title = ascii_title(title or src_path.stem.replace("_", " "))
    length = max(0.1, min(duration or src.duration, src.duration - start))
    folder = dest_root / "MOVIES" / safe_name(title)
    folder.mkdir(parents=True, exist_ok=True)
    log(f"{src_path.name}: {src.width}x{src.height}, {float(src.fps):.3f} fps, "
        f"{length:.1f} s -> {folder}")

    audio = folder / "audio.tmp"
    if src.has_audio:
        subprocess.run([ffmpeg, "-v", "error", "-y", "-ss", str(start), "-t", str(length),
                        "-i", str(src.path), "-vn", "-ac", "2", "-ar", str(AUDIO_RATE),
                        "-f", "s16le", str(audio)], check=True)

    # the modes encode at the same time (one encoder each, a core each)
    done = {m: 0.0 for m in modes}
    errors = []
    lock = threading.Lock()
    plain_log = log

    def log(msg):
        with lock:
            plain_log(msg)

    def encode(mode):
        g = geometry(mode, src)
        lines = g.h
        out_fps = g.fps * (2 if g.temporal else 1)
        frames = int(length * float(g.fps))
        vf = (f"scale={g.w}:{lines}:flags=lanczos,setsar=1,"
              f"fps={out_fps.numerator}/{out_fps.denominator},format=rgb24")
        field_h = lines // 2 if mode == "i480" else lines
        log(f"  {FILE_NAMES[mode]}: {g.w}x{lines} at {float(g.fps):.3f} fps"
            f"{' (fields from successive frames)' if g.temporal else ''}")
        dec = subprocess.Popen([ffmpeg, "-v", "error", "-ss", str(start), "-t", str(length),
                                "-i", str(src.path), "-an", "-vf", vf, "-f", "rawvideo", "-"],
                               stdout=subprocess.PIPE)
        cmd = [stvenc, "--profile", mode, "--size", f"{g.w}x{field_h}", "--pos", f"{g.x},{g.y}",
               "--fps", f"{g.fps.numerator}/{g.fps.denominator}", "--frames", str(frames),
               "--title", title, "--quality", str(quality), "--rate-cap", str(rate_cap),
               "--out", str(folder / FILE_NAMES[mode]), "-v"]
        if g.temporal:
            cmd.append("--temporal-fields")
        if src.has_audio:
            cmd += ["--audio", str(audio), "--audio-rate", str(AUDIO_RATE)]
        enc = subprocess.Popen(cmd, stdin=dec.stdout, stderr=subprocess.PIPE, text=True)
        dec.stdout.close()
        for line in enc.stderr:
            m = re.match(r"chunk \d+: frames \d+-(\d+)", line)
            if m:
                done[mode] = int(m[1]) / max(1, frames)
                if progress:
                    progress(sum(done.values()) / (len(modes) + 0.2))
            elif not line.startswith("palette:"):
                log(f"  {FILE_NAMES[mode]}: " + line.rstrip())
        if enc.wait() != 0:
            errors.append(f"stvenc failed on {src_path.name} ({mode})")
        dec.wait()

    threads = [threading.Thread(target=encode, args=(m,)) for m in modes]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        raise SystemExit("; ".join(errors))

    log("  thumbnails")
    every = max(1, min(THUMB_EVERY_S, round(length / 200)))
    thumbs = grab_frames(ffmpeg, src, every, THUMB_W, THUMB_H, start, length)
    poster_at = start + min(length * 0.1, 60.0)
    poster = grab_frames(ffmpeg, src, 1000, POSTER_W, POSTER_H, poster_at, 1.0)
    write_info(folder / "INFO.STI", title, modes, length, thumbs, every,
               poster[0] if poster else b"\0" * POSTER_W * POSTER_H * 3)
    if audio.exists():
        audio.unlink()
    if progress:
        progress(1.0)
    log("  done")
    return folder


# ---------------------------------------------------------------- window

def run_gui(prefill: list[Path] | None = None) -> None:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    root = tk.Tk()
    root.title("SegaTube Converter")
    files: list[Path] = []
    dest = tk.StringVar()
    q = tk.DoubleVar(value=6)
    mode_vars = {m: tk.BooleanVar(value=m != "i480") for m in PROFILES}   # (480i: on hold)

    frm = ttk.Frame(root, padding=12)
    frm.grid(sticky="nsew")
    ttk.Label(frm, text="Videos to convert").grid(row=0, column=0, sticky="w")
    lst = tk.Listbox(frm, width=60, height=8)
    lst.grid(row=1, column=0, columnspan=3, sticky="nsew")

    def add_files():
        for f in filedialog.askopenfilenames(title="Choose videos"):
            files.append(Path(f))
            lst.insert("end", f)

    def choose_dest():
        d = filedialog.askdirectory(title="Choose the SD card (or any folder)")
        if d:
            dest.set(d)

    for f in prefill or []:
        files.append(f)
        lst.insert("end", str(f))
    ttk.Button(frm, text="Add videos...", command=add_files).grid(row=2, column=0, sticky="w", pady=4)
    ttk.Label(frm, text="SD card:").grid(row=3, column=0, sticky="w")
    ttk.Entry(frm, textvariable=dest, width=44).grid(row=3, column=1, sticky="we")
    ttk.Button(frm, text="Choose...", command=choose_dest).grid(row=3, column=2)
    mf = ttk.Frame(frm)
    mf.grid(row=4, column=0, columnspan=3, sticky="w", pady=4)
    for m, label in (("film", "Film (15-bit colour)"), ("full", "Full (256 colours)"), ("i480", "480i (experimental)")):
        ttk.Checkbutton(mf, text=label, variable=mode_vars[m]).pack(side="left", padx=4)
    ttk.Label(frm, text="Quality").grid(row=5, column=0, sticky="w")
    ttk.Scale(frm, from_=1, to=10, variable=q).grid(row=5, column=1, sticky="we")
    bar = ttk.Progressbar(frm, maximum=1.0)
    bar.grid(row=6, column=0, columnspan=3, sticky="we", pady=6)
    logbox = tk.Text(frm, width=72, height=10)
    logbox.grid(row=7, column=0, columnspan=3)

    def log(msg):
        root.after(0, lambda: (logbox.insert("end", msg + "\n"), logbox.see("end")))

    def go():
        modes = [m for m in PROFILES if mode_vars[m].get()]
        if not files or not dest.get() or not modes:
            messagebox.showinfo("SegaTube", "Add videos, choose the SD card and at least one mode.")
            return

        def work():
            for i, f in enumerate(files):
                try:
                    convert(f, Path(dest.get()), modes, q.get(), 900000, log=log,
                            progress=lambda v, i=i: root.after(0, bar.configure,
                                                               {"value": (i + v) / len(files)}))
                except SystemExit as e:
                    log(f"ERROR: {e}")
            log("All done. Put the SD card back in the EverDrive.")

        threading.Thread(target=work, daemon=True).start()

    ttk.Button(frm, text="Convert", command=go).grid(row=8, column=2, sticky="e")
    root.mainloop()


def main() -> int:
    if len(sys.argv) == 1:
        run_gui()
        return 0
    # videos dropped on the app (no options): the window, with them listed
    if all(not a.startswith("-") and Path(a).is_file() for a in sys.argv[1:]):
        run_gui([Path(a) for a in sys.argv[1:]])
        return 0
    ap = argparse.ArgumentParser(description="Convert videos into SegaTube movies.")
    ap.add_argument("inputs", nargs="+", type=Path)
    ap.add_argument("--sd", type=Path, help="the SD card's root folder")
    ap.add_argument("--out", type=Path, help="a folder (MOVIES/ is made inside it)")
    ap.add_argument("--modes", default="film,full",
                    help="film,full (default); i480 is experimental: the 32X cannot draw one of its fields")
    ap.add_argument("--quality", type=float, default=6)
    ap.add_argument("--rate-cap", type=int, default=900000,
                    help="bytes per second the EverDrive can stream (default 900000)")
    ap.add_argument("--title")
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--duration", type=float)
    ns = ap.parse_args()
    root = ns.sd or ns.out
    if not root:
        ap.error("give --sd or --out")
    modes = [m.strip() for m in ns.modes.split(",") if m.strip()]
    for m in modes:
        if m not in PROFILES:
            ap.error(f"unknown mode {m}")
    for src in ns.inputs:
        convert(src, root, modes, ns.quality, ns.rate_cap, ns.title if len(ns.inputs) == 1 else None,
                ns.start, ns.duration)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
