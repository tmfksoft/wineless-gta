#!/usr/bin/env bash
# Build wineless_libretro.so for an Ubuntu 24.04 (glibc 2.39) deployment target,
# using a disposable builder container so the host's own deps/ (which may target
# a newer glibc) are left untouched.
#
# Why a container: glibc 2.43 (Ubuntu 25.10+/26.04) versioned the single-
# precision math symbols (sqrtf/acosf/...). A core built there will NOT load on
# a 24.04 node ("libm.so.6: version GLIBC_2.43 not found"). DXVK/SDL3 must be
# rebuilt against the target glibc too, which is what this does.
#
# Usage:  scripts/libretro/build-core.sh [output_dir]
# Output: <output_dir>/wineless_libretro.so   (default: ./build-libretro)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build-libretro}"
IMAGE=wineless-builder2404
VOLUME=wl_libretro_tree   # persists built deps across runs (first run is slow)

mkdir -p "$OUT"
docker build -t "$IMAGE" -f "$ROOT/scripts/libretro/Dockerfile.build" "$ROOT/scripts/libretro"
docker volume create "$VOLUME" >/dev/null

docker run --rm \
  -v "$ROOT":/src-ro:ro \
  -v "$ROOT/scripts/libretro/in_container_build.sh":/build.sh:ro \
  -v "$VOLUME":/tree \
  -v "$OUT":/out \
  "$IMAGE" bash /build.sh

echo "==> $OUT/wineless_libretro.so"
