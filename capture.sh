#!/usr/bin/env bash
#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# capture.sh — grab the board's video through the HDMI capture stick.
#
# The board's VGA ladder feeds an MS9288A (U16) which drives the HDMI
# connector J12; a MACROSILICON MS21xx USB capture stick on the other end
# of that cable enumerates as "USB Video" and gives 640x480@30 in UYVY.
# That is enough to read an 80x25 text screen, which makes "did it boot"
# an answerable question from a script rather than something to lean over
# the bench for.
#
# Usage:
#   ./capture.sh                     one frame to out/screen.png
#   ./capture.sh shot out/foo.png    one frame, named
#   ./capture.sh clip 10 out/b.mp4   ten seconds of video
#   ./capture.sh view                live preview
#
set -euo pipefail
cd "$(dirname "$0")"

DEVICE="${CAPTURE_DEVICE:-USB Video}"
SIZE="${CAPTURE_SIZE:-640x480}"
FPS="${CAPTURE_FPS:-30}"

# The stick keeps sending the last frame it saw for a moment after the
# source changes, and its first frames after opening are often the tail of
# the previous mode. Discarding a few makes a shot taken right after a
# reset show what is on the screen now.
WARMUP="${CAPTURE_WARMUP:-8}"

common=(-hide_banner -loglevel error -f avfoundation -framerate "${FPS}" -video_size "${SIZE}" -i "${DEVICE}")

mkdir -p out

case "${1:-shot}" in
    shot)
        OUT="${2:-out/screen.png}"
        ffmpeg "${common[@]}" -vf "select=gte(n\,${WARMUP})" -frames:v 1 -update 1 -y "${OUT}"
        echo "${OUT}"
        ;;
    clip)
        SECS="${2:-5}"
        OUT="${3:-out/capture.mp4}"
        ffmpeg "${common[@]}" -t "${SECS}" -c:v libx264 -pix_fmt yuv420p -y "${OUT}"
        echo "${OUT}"
        ;;
    view)
        exec ffplay -hide_banner -loglevel error \
            -f avfoundation -framerate "${FPS}" -video_size "${SIZE}" -i "${DEVICE}"
        ;;
    modes)
        # The stick reports its whole mode list when asked for one it does
        # not have, which is the only way to enumerate it through ffmpeg.
        ffmpeg -hide_banner -f avfoundation -video_size 99999x99999 -i "${DEVICE}" 2>&1 \
            | sed -n 's/^.*Supported modes:/Supported modes:/p;s/^\[avfoundation[^]]*\] *\( *[0-9]\+x[0-9]\+@.*\)$/\1/p'
        ;;
    *)
        echo "usage: $0 [shot [file] | clip [secs] [file] | view | modes]" >&2
        exit 2
        ;;
esac
