# Wineless

Run older Windows games on 64-bit Linux **without Wine or Proton**. `wineless-gta` is
a single, self-contained executable: drop it into a game's folder, run it, and the game
starts as if it were a native Linux program.

It is built and tuned first for **Grand Theft Auto III, Vice City and San Andreas**, and
has also been tested against a handful of other games of that era (for example Tomb
Raider III). Anything else is fair game to try, but there are no guarantees.

```sh
# simplest case: put wineless-gta in the game folder and run it
cd "/path/to/GTA San Andreas"
./wineless-gta
```

It finds `gta_sa.exe`, `gta-vc.exe`, `gta3.exe`, or another `gta*.exe` automatically; any
other game is selected with `--exe`.

Prebuilt binaries for 64-bit x86 Linux are on the
[Releases](../../releases) page; or [build from source](#building).

## Scope and limitations

**64-bit x86 Linux only.** Wineless runs the game's 32-bit x86 instructions directly on the
host CPU (in its 32-bit compatibility mode) — it does **not** emulate or translate CPU
instructions. It therefore requires a 64-bit x86 (x86-64 / AMD64) machine. It does **not**
run on ARM (including Apple Silicon and ARM Chromebooks/SBCs), RISC-V, or any non-x86
architecture, and there is currently no plan to add instruction emulation for them.

**This is not a general Windows compatibility layer.** It implements only as much of
Windows as the targeted games actually use, and it is missing large parts of the Windows
API that mature projects like Wine and Proton provide. Many programs will not run, or will
run only partially. Treat it as a focused tool for a small set of older games, not a
drop-in Wine replacement.

**This is not a DRM or copy-protection bypass.** Wineless does not remove, strip, patch
or circumvent any protection. Games run exactly as shipped: their DRM, copy-protection,
licensing and integrity checks execute unmodified. Whether such a game runs depends
entirely on whether Wineless provides enough genuine Windows and Steam behaviour for those
checks to complete on their own — the same way they would on a real Windows machine. If a
protected game does not run, that is a gap in what Wineless emulates, not a defeated check.

You must own the games you run, and a game's own requirements (such as needing its disc,
or a running, logged-in Steam client) still apply.

## Known games

This list is small and grows as games are tried. Anything not listed is simply untested.
"Works" means it reaches gameplay; it does not promise every feature is perfect.

### Working

| Game | Notes |
|---|---|
| **GTA: San Andreas** | The primary target; reaches gameplay. |
| **GTA III** | A primary target. The Steam build loads, plays the intros and reaches the main menu, rendering steadily. |
| **GTA: Vice City** | A primary target. The Steam build loads, passes its disc check, plays the intros and reaches the main menu. |
| **Tomb Raider III** (`--exe tomb3.exe`) | Runs, including its FMVs and music. |
| **Age of Empires II HD** | Reaches the menus and gameplay; some GDI UI text still renders imperfectly. |

### Partial / not working yet

| Game | Status |
|---|---|
| **Rayman Legends** | Requires Uplay, which Wineless does not provide. |

When a game depends on an external launcher or service Wineless doesn't implement (Uplay,
EA app, GOG Galaxy, a bespoke online check, …), it won't run until that dependency is met —
these are missing-environment gaps, not defeated checks (see
[Scope and limitations](#scope-and-limitations)).

## How it works

`wineless-gta` is a single 64-bit executable. It loads the game's 32-bit code into the
low 2 GB of its own address space and runs it directly in the CPU's 32-bit compatibility
mode. Each Windows API call the game makes becomes a thunk into host C code:

- **Graphics:** Direct3D 8 and 9 via a statically linked [DXVK](https://github.com/doitsujin/dxvk),
  so Vulkan does the rendering. Older DirectDraw / Direct3D 5 calls are drawn through the
  same DXVK device.
- **Window, input and audio:** SDL3, statically linked.
- **Ogg/Vorbis audio:** static libvorbis.
- **The game's own DLLs** (for example Miles Sound System in GTA III and Vice City) are
  loaded from disk, as on Windows.

The only dynamic dependency is glibc. Vulkan, X11/Wayland and the audio backends are
loaded from the system at runtime.

## Building

```sh
scripts/build-deps.sh          # meson, glslang, SDL3, libogg/libvorbis, DXVK -> ./deps
cmake -S . -B build -G Ninja
ninja -C build
```

## Running

```sh
./build/wineless-gta --game-dir "/path/to/GTA San Andreas"
```

- `--exe NAME` — the game executable to run. Omit it for a GTA game; use it for anything else.
- `--game-dir DIR` — the folder containing the game. Falls back to `$WINELESS_GAME_DIR`,
  then the binary's own folder, then the current folder.
- `--user-dir DIR` — where saves and settings go. Defaults to
  `<XDG Documents>/<Game> User Files`.

File lookups inside the game folder are case-insensitive, using an index built at startup,
so Windows-style paths resolve regardless of case.

### One-click launch: `wineless.config`

Put `wineless-gta` in a game folder next to a `wineless.config` file and run it with no
arguments. The file is read from the binary's own directory. It uses `key = value` lines,
with `#` or `;` for comments. Known keys are `exe`, `game-dir`, `user-dir` and `display`;
any other key sets an environment variable. Command-line flags and real environment
variables override the file. `~` and relative paths resolve against the binary's folder.

```ini
# wineless.config
exe          = AoK HD.exe
game-dir     = .
user-dir     = ~/Documents/AoE2 Saves
display      = fullscreen
WINELESS_LOG = info
```

### Display

The game's own display/resolution menu lists your monitor's real resolutions (32-bit
color only). How a game's fullscreen request is presented is controlled by `--display`
(or `$WINELESS_DISPLAY`):

- `fullscreen` (default) — a real fullscreen mode at the chosen resolution. On Wayland the
  compositor emulates it with scaling.
- `borderless` — a desktop-sized borderless window with the game's image scaled to fill it.
- `windowed` (or `--windowed`) — a normal window at the chosen resolution.

GTA: San Andreas stores its resolution as a position in that list. If your monitor's mode
list changes, or you copy a `gta_sa.set` over from Wine, re-pick the resolution in the menu.

## Compatibility notes

- **Mods (GTA: SA):** the classic ASI loader (a `vorbisFile.dll` with the real one renamed
  to `vorbisHooked.dll`) is handled automatically, so ASI plugins such as SilentPatch load.
  ASI loaders that pose as system DLLs (`dinput8.dll`, `winmm.dll`, …) placed in the game
  folder are loaded in place of the built-in DLL, exactly as on Windows.
- **Intro movies** (`movies/*.mpg`, MPEG-1) are decoded with
  [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (MIT, vendored in
  `src/third_party/pl_mpeg.h`). They play on their own thread and a key press skips them.
- **GDI text** is rasterized with [stb_truetype](https://github.com/nothings/stb)
  (public domain / MIT, vendored in `src/third_party/stb_truetype.h`), using the fonts the
  game ships and then the system fonts. Windows faces without a Linux copy map to
  metric-compatible ones (Arial → Liberation Sans, Times New Roman → Liberation Serif, …).
- **Gamepads:** SDL gamepads appear to the game as DirectInput controllers laid out like an
  Xbox pad under DirectInput on Windows (left stick X/Y, both triggers on Z, right stick
  Rx/Ry, buttons A B X Y LB RB Back Start L3 R3, D-pad as the hat). Configure the in-game
  controls the same way you would on Windows.

## Steam

Steam games that use `steam_api.dll` talk to your normal Linux Steam client: a built-in
`steam_api` loads Steam's own `steamclient.so` — the same approach Proton takes — so login,
ownership, achievements, cloud saves and multiplayer are handled by Steam itself. Steam
must be running and you must own the game. As noted in [Scope and limitations](#scope-and-limitations),
any copy-protection checks a Steam game performs run unchanged; Wineless does not bypass
them, so a title whose checks need Windows behaviour Wineless does not provide may not run.

To launch games from Steam, register Wineless as a compatibility tool:

```sh
./build/wineless-gta --install-steam-tool
```

Restart Steam, then choose **Wineless** under a game's *Properties → Compatibility*. Steam
then runs the game through Wineless with the correct app ID. Without a terminal, the log
goes to `~/.cache/wineless-gta/<exe>.log`. Known launchers that only show a splash screen
(for example Age of Empires II HD's `Launcher.exe`) are skipped in favour of the game; for
others, set `WINELESS_EXE="Game.exe" %command%` in the launch options.

Run from a terminal instead and a Steam game uses the app ID it asks for (as
`steam_appid.txt` does for native games).

The interface tables in `src/steam/steam_ifaces.inc` are generated by
`scripts/gen_steam.py` from the Steamworks SDK headers (Proton keeps every version under
`lsteamclient/steamworks_sdk_*`).

## Debug environment variables

| Variable | Effect |
|---|---|
| `WINELESS_LOG=error\|warn\|info\|debug\|trace` | Log level. `trace` logs every API call. |
| `WINELESS_SHOT=100,500` | Save the back buffer to `wineless_shot_<frame>.bmp` at those frames. |
| `WINELESS_SHOT_DIR=dir` | Folder for those screenshots. |
| `WINELESS_MOVIE_SHOT=100,400` | Save those intro-movie frames, numbered across both movies. |
| `WINELESS_KEYS=18000:1c,19500:d0` | Press a DIK key code (hex) at a time in ms since start, held 120 ms. |
| `WINELESS_VIRTUAL_PAD=1` | Attach a virtual gamepad, for testing without a real one. |
| `WINELESS_PAD=19000:down,20400:a` | Press virtual pad buttons at those times in ms (a b x y lb rb back start up down left right). |
| `WINELESS_CLOSE=20000` | Ask the window to close at that time in ms (tests the shutdown path). |
| `WINELESS_WATCHDOG=1` | Dump the main thread's state if it makes no API calls for 5 s. |
| `WINELESS_WATCHDOG=sample` | Dump every guest thread every 2 s. |
| `WINELESS_BACKGROUND=1` | Keep the game running when its window loses focus. |
| `WINELESS_DLL_OVERRIDES=vorbisfile=native` | Load a DLL from disk instead of the built-in version (`=builtin` reverses it). |
| `WINELESS_EXE=Game.exe` | Under the Steam tool, run this exe (in the same folder) instead of the one Steam names. |
| `SDL_VIDEO_DRIVER=wayland` | Use native Wayland. The default is X11/XWayland first. |
