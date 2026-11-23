# CLAUDE.md — working notes for Wineless

Guidance for AI agents (and humans) working in this repo. Read this before making
changes. The user-facing overview is in `README.md`; this file is about *how to
work here safely*.

## What this is

Wineless is a single static 64-bit Linux binary (`wineless-gta`) that runs 32-bit
Windows games directly — no Wine, no Proton. It loads the PE itself, runs the
32-bit guest in CPU compatibility mode, and implements just enough of Win32 to
run the targeted games. Rendering goes through a static DXVK (D3D8/D3D9) and a
separate DirectDraw/Direct3D-5 layer; windowing/input/audio via static SDL3. The
only dynamic dependency is glibc.

- Primary targets: **GTA III, Vice City, San Andreas**. Also exercised against
  **Tomb Raider III** (DirectDraw) and, in progress, **Age of Empires 1 / Rise of
  Rome** (8-bit DirectDraw).
- It is **not** a DRM bypass; copy-protection checks are left intact.
- 64-bit x86 only. No ARM. No `CreateProcess` (single process), so self-extracting
  installers can't run — install under Wine, then run the installed game here.

## Build & run

```sh
cmake --build build            # after first `cmake -S . -B build`
./build/wineless-gta --game-dir <dir> --exe <name.exe>
```
A bare run auto-detects a GTA exe in the binary's folder or `$WINELESS_GAME_DIR`.
A `wineless.config` next to the binary (see `--save`) sets exe/game-dir/user-dir/
display and env vars for drop-in launches.

## WORKING RULES (the user asked for these explicitly)

1. **Never say something is "fixed" / "working" until the user has independently
   confirmed it on their machine.** Describe what changed and what you *expect*;
   let them verify. Don't declare victory from a log line or a clean build.
2. **Run regression checks against the core games after any change that could
   touch shared code** (rendering, GDI, DirectDraw, memory/heap, PE loader, the
   message loop, core Win32). Use `scripts/regression.sh`. The core set is
   **GTA III, Vice City, San Andreas, Tomb Raider III** — each has its own quirks
   and any of them can be broken by a "general" change.
3. Keep changes narrow and reversible. If a fix regresses a core game, prefer
   reverting to the last state the user confirmed over shipping a worse one.

## Debugging & testing tools (BUILT IN — use these, don't reinvent)

All are environment variables. Times are in milliseconds of guest tick unless
noted; frame numbers are Present/Flip counts.

| Var | Purpose |
|-----|---------|
| `WINELESS_LOG=error\|warn\|info\|debug\|trace` | log verbosity |
| `WINELESS_SHOT="10,60,300"` | save a BMP screenshot at those present frames |
| `WINELESS_SHOT_DIR=/path` | where shots go (default cwd) |
| `WINELESS_MOVIE_SHOT=1` | save every frame (for capturing motion) |
| `WINELESS_CLICKS="3000:400:300,..."` | left-click at `time_ms:x:y` (game-res coords) — drive menus |
| `WINELESS_KEYS="2000:1C,..."` | press DirectInput key `DIK` (hex) at `time_ms` — e.g. `1C`=Enter, `01`=Esc |
| `WINELESS_CLOSE=15000` | request window close at `time_ms` (clean exit for scripted runs) |
| `WINELESS_WATCHDOG=1` | dump the main thread's stack if it makes no API call for 5s (finds hangs) |
| `WINELESS_WATCHDOG=sample` | periodically dump all guest threads |
| `WINELESS_DISPLAY=fullscreen\|borderless\|windowed` | window mode |
| `WINELESS_BACKGROUND=1` | keep running when the window loses focus |
| `WINELESS_DLL_OVERRIDES=...` | force builtin vs native DLL resolution |
| `WINELESS_PAD` / `WINELESS_VIRTUAL_PAD` | gamepad mapping / synthesize a virtual pad |
| `WINELESS_EXE` / `WINELESS_GAME_DIR` | override exe / game dir |

**Screenshots are the fastest way to see what actually rendered** — read the BMP
(convert with PIL/ffmpeg; the Read tool can view a PNG). Use `WINELESS_CLICKS`/
`WINELESS_KEYS` + `WINELESS_CLOSE` to script a game past menus without a human.

Note: a purely headless/automated run may not get window focus from the
compositor, and some games spin in their message loop until `WM_ACTIVATEAPP`;
D-Bus calls in SDL's event pump can also add latency that a real desktop doesn't
see. So "it stalls for me" in an automated run is **not** proof it stalls for the
user — confirm with them.

## Regression script

`scripts/regression.sh [wineless-binary]` boots each core game, screenshots a
frame, and reports RENDER/BLACK/NO-FRAME/CRASH. Game paths are hard-coded to this
machine — edit `GAMES` for another setup. Limitations in a headless/CI-like
session: GTA III/VC use Steam-wrapped exes that need Steam running (`gta3.exe`
shows "Application load error 5"); only TR3 is reliably checkable without a full
desktop/Steam session. Treat a clean TR3 render as "DirectDraw/GDI path intact"
but still ask the user to confirm the GTA trio.

## Known game quirks

- **GTA San Andreas** — the easiest; DXVK D3D9. Good smoke-test baseline.
- **GTA III / Vice City** — Steam-wrapped exes hit Steam DRM when run bare; need
  Steam/steamclient or a DRM-free exe. VC also had a drive-relative-path "no CD"
  bug (fixed in `vfs.c`) and the PE loader `.text`-truncation bug (fixed in
  `pe.c`) that surfaced as Steam "error D".
- **Tomb Raider III** — DirectDraw/D3D5 path, 16-bit. Uses the DirectDraw
  surface-DC GDI path. Best canary for DirectDraw/GDI changes.
- **Age of Empires 1 / Rise of Rome** — 8-bit palettized DirectDraw. DRS archives
  are memory-mapped (`MapViewOfFile`); the game calls `VirtualFree(MEM_DECOMMIT)`
  on pointers inside those views, which on Windows fails harmlessly — see the
  `LM_TAG_MAPPED` handling in `lowmem`/`VirtualFree` so we don't unmap live data.
  Uses VC6 static CRT (`__sbh` small-block heap). **Open issue: menu text does not
  render** (whole menu shows, labels blank). The GDI text path (`text_out` →
  `dc_target` in `src/win32/gdi.c`) has no case for a DirectDraw *surface* DC
  (only memory DCs and bitmaps), so text drawn straight onto a surface is dropped;
  `BitBlt` has its own surface path. A first attempt (full-surface RGBA mirror
  read-on-draw / write-on-ReleaseDC) **regressed AoE to a black screen and dimmed
  TR3**, and was reverted — do not reapply that approach without confirming the
  surface read returns live content and without a full regression pass. This is
  not yet solved.

## Memory files

Project memory lives in
`~/.claude/projects/-mnt-data-Development-Tests-SA-Wrapper/memory/` with an index
in `MEMORY.md`. The AoE2 text-rendering investigation is documented in
`docs/aoe2-text-rendering.md`.
