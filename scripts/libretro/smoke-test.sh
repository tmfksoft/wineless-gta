#!/usr/bin/env bash
# Smoke-test a wineless_libretro.so by loading it in the real retrolive frontend
# (Ubuntu 24.04 / FFmpeg 6 runtime) and recording a short mp4, then extracting
# frames. A headless X server (Xvfb) + software Vulkan (lavapipe) give the
# Wineless guest's DXVK a surface without a GPU.
#
# Usage:
#   scripts/libretro/smoke-test.sh <core.so> <retrolive_dir> [out_dir] [game.exe] [duration_s]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CORE="$(readlink -f "${1:?core .so path required}")"
RL="$(readlink -f "${2:?retrolive dir required}")"
OUT="$(readlink -f "${3:-$ROOT/build-libretro/smoke}")"
GAME="${4:-}"
DUR="${5:-20}"
IMAGE=wineless-retrolive-harness

mkdir -p "$OUT"
docker build -t "$IMAGE" -f "$ROOT/scripts/libretro/Dockerfile.harness" "$ROOT/scripts/libretro" >/dev/null

GAME_MOUNT=() ; GAME_ARGS=()
if [ -n "$GAME" ]; then
  GAME="$(readlink -f "$GAME")"
  GAME_MOUNT=(-v "$(dirname "$GAME")":/game:ro)
  GAME_ARGS=(--game "/game/$(basename "$GAME")")
fi

# In-container wrapper: start Xvfb sized to the game's resolution (so a
# fullscreen game doesn't try a mode switch Xvfb can't do), point the guest's
# SDL3 at it, run retrolive.
WRAP='set -e
Xvfb :99 -screen 0 '"${XVFB_RES:-640x480}"'x24 >/tmp/xvfb.log 2>&1 &
for i in $(seq 1 50); do [ -e /tmp/.X11-unix/X99 ] && break; sleep 0.1; done
export DISPLAY=:99
export SDL_VIDEO_DRIVER=x11
exec "$@"'

rm -f "$OUT/test.mp4"
docker run --rm \
  -e LD_LIBRARY_PATH=/rl -e SDL_VIDEODRIVER=offscreen \
  -e WINELESS_DISPLAY="${WINELESS_DISPLAY:-windowed}" \
  -e WINELESS_KEYS="${WINELESS_KEYS:-}" -e WINELESS_CLICKS="${WINELESS_CLICKS:-}" \
  -e WINELESS_LOG="${WINELESS_LOG:-info}" \
  -v "$RL":/rl:ro -v "$(dirname "$CORE")":/work:ro -v "$OUT":/out \
  "${GAME_MOUNT[@]}" \
  "$IMAGE" bash -c "$WRAP" _ \
    /rl/retrolive --core "/work/$(basename "$CORE")" \
      "${GAME_ARGS[@]}" \
      --video /out/test.mp4 --width 640 --height 480 --duration "$DUR" --verbose

docker run --rm -v "$OUT":/out "$IMAGE" bash -c '
  ffprobe -v error -select_streams v:0 -count_frames \
    -show_entries stream=nb_read_frames,width,height -of default=nw=1 /out/test.mp4 || true'
echo "==> $OUT/test.mp4"
