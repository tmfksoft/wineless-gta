# Wineless libretro core

`wineless_libretro.so` exposes Wineless through the [libretro](https://www.libretro.com/)
C ABI so a libretro-hosting frontend (e.g. the retrolive cloud-gaming frontend)
can run the Windows games Wineless supports and stream them.

It is a **second build target** in the top-level `CMakeLists.txt`
(`-DBUILD_LIBRETRO=ON`, on by default). The standalone `wineless-gta`
executable is unaffected.

## Status: Phase 2 (real guest, DirectDraw path)

- **P1 (done):** loadable `.so`, correct `retro_*` ABI, XRGB8888 frames through
  the frontend to an encoded mp4.
- **P2 (done for the DirectDraw path):** the guest boots on its own thread and
  its frames reach the frontend via a **present-barrier**. At each present the
  `wl_present_hook` (see `src/present_hook.h`) reads the backbuffer back to CPU
  pixels, feeds `video_refresh`, and blocks to pace the guest; `retro_run`
  consumes the latest frame and releases it. **Tomb Raider III streams through
  the real retrolive binary at ~60fps.** Only the DirectDraw present chokepoint
  (`ddg_present`) is wired so far — **GTA III/VC/SA (D3D8/D3D9) are not hooked
  yet**, so they produce no frames in the core. Input/audio are still stubbed.
- **P3 (next):** route libretro input into the existing injection sinks
  (`dinput8`/`user32` mouse/`dinput_pad`); drain the dsound mix in `retro_run`;
  wire the D3D8/D3D9 present paths.
- **P4:** regression pass on the core games.

Save states are unsupported by design (a live Win32 process can't be snapshotted
deterministically); `retro_serialize_size()` returns 0. In-game saves still work.

### Headless requirements

DXVK needs a real Vulkan surface even for the DirectDraw path, so a headless host
needs a virtual X server and software Vulkan: the smoke test starts **Xvfb**
(sized to the game's resolution) and uses **lavapipe**, and runs the guest
windowed (`WINELESS_DISPLAY=windowed`) because Xvfb can't change display modes.
On a GPU node, use the real display/driver instead.

## Build (for an Ubuntu 24.04 deployment target)

```sh
scripts/libretro/build-core.sh [out_dir]      # -> out_dir/wineless_libretro.so
```

Builds inside a disposable Ubuntu 24.04 container. **This matters:** glibc 2.43
(Ubuntu 25.10+) versioned the single-precision math symbols, so a core built on
a 2.43 host will not load on a 2.39 node. The container rebuilds DXVK/SDL3/vorbis
against the target glibc. First run is slow (DXVK); a Docker volume caches the
built deps for subsequent runs.

For a host-native build (runs only where glibc >= the build host's):

```sh
cmake -S . -B build -DBUILD_LIBRETRO=ON && cmake --build build --target wineless_libretro
```

## Smoke-test (no GPU needed)

```sh
scripts/libretro/smoke-test.sh <core.so> <retrolive_unpacked_dir> [out_dir] [game.exe]
```

Loads the core in the real retrolive binary (Ubuntu 24.04 / FFmpeg 6 runtime
container), records a 3s mp4, and extracts two frames. For the P1 skeleton, omit
`game.exe` — it renders the test pattern.
