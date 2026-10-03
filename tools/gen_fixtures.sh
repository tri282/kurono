#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tools/gen_fixtures.sh -- regenerate tools/fixtures/ for tools/gapless_test,
# or with --listen DIR write 10 s + 10 s pairs for a by-ear gapless check.

set -eu

F0=200
F1=6000

need() {
  ffmpeg -hide_banner -encoders 2>/dev/null | grep -q " $1 " \
    || { echo "gen_fixtures.sh: missing encoder: $1" >&2; exit 1; }
}

# enc OUT RATE START END TOTAL ARGS...: frames [START, END) of a TOTAL-frame
# chirp at RATE, encoded with ARGS.
enc() {
  out=$1 rate=$2 s=$3 e=$4 total=$5
  shift 5
  T=$(awk "BEGIN { printf \"%.9f\", $total / $rate }")
  x="0.5*sin(2*PI*($F0*t+($F1-$F0)*t*t/(2*$T)))"
  ffmpeg -hide_banner -loglevel error -y -f lavfi \
    -i "aevalsrc=exprs=$x|$x:s=$rate:c=stereo,atrim=start_sample=$s:end_sample=$e,asetpts=N/SR/TB" \
    "$@" "$out"
}

# pair OUT_A OUT_B RATE NA NB ARGS...: one chirp split at frame NA.
pair() {
  a=$1 b=$2 rate=$3 na=$4 nb=$5
  shift 5
  enc "$a" "$rate" 0 "$na" $((na + nb)) "$@"
  enc "$b" "$rate" "$na" $((na + nb)) $((na + nb)) "$@"
}

for c in libmp3lame aac libopus libvorbis; do need "$c"; done

MP3="-c:a libmp3lame -b:a 128k"
M4A="-c:a aac -b:a 128k"
OPUS="-c:a libopus -b:a 96k"
OGG="-c:a libvorbis -q:a 4"

if [ "${1:-}" = "--listen" ]; then
  d=${2:?usage: gen_fixtures.sh --listen DIR}
  for set in mp3 m4a opus ogg; do
    mkdir -p "$d/$set"
  done
  pair "$d/mp3/1-a.mp3"   "$d/mp3/2-b.mp3"   48000 480000 480000 $MP3
  pair "$d/m4a/1-a.m4a"   "$d/m4a/2-b.m4a"   48000 480000 480000 $M4A
  pair "$d/opus/1-a.opus" "$d/opus/2-b.opus" 48000 480000 480000 $OPUS
  pair "$d/ogg/1-a.ogg"   "$d/ogg/2-b.ogg"   48000 480000 480000 $OGG
  exit 0
fi

d=$(dirname "$0")/fixtures
mkdir -p "$d"
pair "$d/mp3-a.mp3"     "$d/mp3-b.mp3"     48000 24337 21111 $MP3
pair "$d/m4a-a.m4a"     "$d/m4a-b.m4a"     48000 24337 21111 $M4A
pair "$d/opus-a.opus"   "$d/opus-b.opus"   48000 24337 21111 $OPUS
pair "$d/ogg-a.ogg"     "$d/ogg-b.ogg"     48000 24337 21111 $OGG
pair "$d/mp3-44k-a.mp3" "$d/mp3-44k-b.mp3" 44100 22337 19411 $MP3
enc  "$d/mp3-noxing.mp3" 48000 0 24337 24337 $MP3 -write_xing 0
enc  "$d/adts.aac"       48000 0 24337 24337 $M4A -f adts
