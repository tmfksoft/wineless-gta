#!/usr/bin/env bash
# Fetch and build third-party dependencies into ./deps (no root needed).
#   - meson/ninja (python venv)
#   - glslang (DXVK compiles its shaders with it at build time)
#   - DXVK (native Linux build, D3D9 only)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/deps"
SRC="$DEPS/src"
PREFIX="$DEPS/prefix"
JOBS="${JOBS:-$(nproc)}"
DXVK_TAG="${DXVK_TAG:-v2.7.1}"
GLSLANG_TAG="${GLSLANG_TAG:-15.1.0}"

mkdir -p "$SRC" "$PREFIX"

# ---- meson
if [ ! -x "$DEPS/venv/bin/meson" ]; then
  python3 -m venv "$DEPS/venv"
  "$DEPS/venv/bin/pip" install -q meson
fi
export PATH="$DEPS/venv/bin:$PREFIX/bin:$PATH"

# ---- glslang
if [ ! -x "$PREFIX/bin/glslangValidator" ]; then
  [ -d "$SRC/glslang" ] || git clone -q --depth 1 -b "$GLSLANG_TAG" https://github.com/KhronosGroup/glslang "$SRC/glslang"
  cmake -S "$SRC/glslang" -B "$SRC/glslang/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DENABLE_HLSL=OFF >/dev/null
  ninja -C "$SRC/glslang/build" -j "$JOBS" install >/dev/null
fi

# ---- SDL3 (static; X11/Wayland/Pulse/ALSA are dlopen'ed at runtime)
SDL_TAG="${SDL_TAG:-release-3.4.8}"
if [ ! -f "$PREFIX/lib/libSDL3.a" ]; then
  [ -d "$SRC/SDL" ] || git clone -q --depth 1 -b "$SDL_TAG" https://github.com/libsdl-org/SDL "$SRC/SDL"
  cmake -S "$SRC/SDL" -B "$SRC/SDL/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_DEPS_SHARED=ON -DSDL_X11_XTEST=OFF -DSDL_CAMERA=OFF -DSDL_GPU=OFF -DSDL_RENDER=OFF >/dev/null
  ninja -C "$SRC/SDL/build" -j "$JOBS" install >/dev/null
  # pkg-config needs spaces in paths escaped
  sed -i '/^prefix=/s/ /\\ /g' "$PREFIX/lib/pkgconfig/sdl3.pc"
fi

# ---- libogg + libvorbis (static) for the game's Ogg audio streams
if [ ! -f "$PREFIX/lib/libvorbisfile.a" ]; then
  [ -d "$SRC/ogg" ] || git clone -q --depth 1 -b v1.3.6 https://github.com/xiph/ogg "$SRC/ogg"
  cmake -S "$SRC/ogg" -B "$SRC/ogg/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DINSTALL_DOCS=OFF -DBUILD_TESTING=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
  ninja -C "$SRC/ogg/build" -j "$JOBS" install >/dev/null
  [ -d "$SRC/vorbis" ] || git clone -q --depth 1 -b v1.3.7 https://github.com/xiph/vorbis "$SRC/vorbis"
  cmake -S "$SRC/vorbis" -B "$SRC/vorbis/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
  ninja -C "$SRC/vorbis/build" -j "$JOBS" install >/dev/null
fi

# ---- DXVK
if [ ! -d "$SRC/dxvk" ]; then
  git clone -q --depth 1 -b "$DXVK_TAG" --recurse-submodules --shallow-submodules \
    https://github.com/doitsujin/dxvk "$SRC/dxvk"
  if [ -d "$ROOT/patches/dxvk" ]; then
    for p in "$ROOT"/patches/dxvk/*.patch; do
      [ -e "$p" ] || continue
      git -C "$SRC/dxvk" apply "$p"
    done
  fi
fi
if [ ! -f "$SRC/dxvk/build/src/d3d8/libdxvk_d3d8.a" ]; then
  rm -rf "$SRC/dxvk/build"
  PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" meson setup "$SRC/dxvk/build" "$SRC/dxvk" \
    --buildtype=release -Ddefault_library=static -Db_pie=true \
    -Denable_dxgi=false -Denable_d3d8=true -Denable_d3d10=false -Denable_d3d11=false \
    -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled \
    -Dcpp_args="-DDXVK_SDL3_STATIC" >/dev/null
  ninja -C "$SRC/dxvk/build" -j "$JOBS"
fi
echo "deps ready in $DEPS"
