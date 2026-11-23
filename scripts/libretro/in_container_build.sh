#!/usr/bin/env bash
set -euo pipefail
echo "[builder] syncing source (excluding host deps/build)..."
rsync -a --delete \
  --exclude '/deps' --exclude '/build' --exclude '.git' \
  /src-ro/ /tree/
cd /tree
echo "[builder] building deps (glslang/SDL3/ogg/vorbis/DXVK) against glibc $(ldd --version|head -1|grep -oE '[0-9]+\.[0-9]+')..."
JOBS="$(nproc)" bash scripts/build-deps.sh
echo "[builder] configuring + building libretro core..."
cmake -S /tree -B /tree/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_LIBRETRO=ON >/dev/null
cmake --build /tree/build --target wineless_libretro -j"$(nproc)"
echo "[builder] checking glibc symbol floor of the produced core:"
objdump -T /tree/build/wineless_libretro.so | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -3
cp /tree/build/wineless_libretro.so /out/wineless_libretro.so
echo "[builder] DONE -> /out/wineless_libretro.so"
