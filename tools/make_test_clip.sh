#!/bin/sh
# A short generated test clip for the test ROM and the tests (no copyrighted
# footage): a zooming fractal beside moving colour bars, a tone and noise.
set -e
out="$1"
mkdir -p "$(dirname "$out")"
ffmpeg -v error -y \
  -f lavfi -i "mandelbrot=size=640x360:rate=30000/1001,format=rgb24" \
  -f lavfi -i "testsrc2=size=640x360:rate=30000/1001" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000,volume=0.3" \
  -f lavfi -i "anoisesrc=color=pink:amplitude=0.1:sample_rate=48000" \
  -filter_complex "[0:v][1:v]blend=all_expr='if(lt(X,W/2),A,B)'[v];[2:a][3:a]amix=inputs=2,pan=stereo|c0=c0|c1=c0[a]" \
  -map "[v]" -map "[a]" -t 6 -c:v libx264 -pix_fmt yuv420p -c:a aac "$out"
