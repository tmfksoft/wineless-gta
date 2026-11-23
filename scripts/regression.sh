#!/usr/bin/env bash
# Regression smoke test for the core games Wineless targets.
#
# Boots each known game headlessly for a few seconds, captures a frame with
# WINELESS_SHOT, and reports whether it rendered something (not a black frame)
# and whether it crashed. Run this after any change that touches the shared
# render/GDI/DirectDraw/memory paths.
#
# Usage: scripts/regression.sh [path-to-wineless-gta]
# Game locations are this machine's; edit GAMES for another setup.
set -u

WL="${1:-$(dirname "$0")/../build/wineless-gta}"
OUT="$(mktemp -d /tmp/wineless-regression.XXXXXX)"
SHOT_FRAMES="90,180,300"

# name | game-dir | exe
GAMES=(
  "GTA3|/home/thomas/.local/share/Steam/steamapps/common/Grand Theft Auto 3|gta3.exe"
  "ViceCity|/home/thomas/.local/share/Steam/steamapps/common/Grand Theft Auto Vice City|gta-vc.exe"
  "SanAndreas|/home/thomas/Games/gtasa/drive_c/Program Files (x86)/Rockstar Games/GTA San Andreas|gta_sa.exe"
  "TombRaider3|/home/thomas/.local/share/Steam/steamapps/common/TombRaider (III)|tomb3.exe"
)

echo "wineless-gta: $WL"
echo "shots: $OUT"
echo

brightness() { # mean luma of a bmp, 0-255, or "-" if missing
  python3 - "$1" <<'PY' 2>/dev/null || echo "-"
import sys
from PIL import Image
im=Image.open(sys.argv[1]).convert("L")
px=list(im.getdata())
print(round(sum(px)/len(px),1))
PY
}

for g in "${GAMES[@]}"; do
  IFS='|' read -r name dir exe <<<"$g"
  if [ ! -f "$dir/$exe" ]; then printf "%-14s SKIP (not found)\n" "$name"; continue; fi
  sd="$OUT/$name"; mkdir -p "$sd"
  timeout 25 env WINELESS_LOG=warn WINELESS_SHOT="$SHOT_FRAMES" WINELESS_SHOT_DIR="$sd" \
    "$WL" --game-dir "$dir" --exe "$exe" >"$sd/log.txt" 2>&1
  rc=$?
  crash=""; grep -q "signal 11\|UNIMPLEMENTED" "$sd/log.txt" && crash=" CRASH/UNIMPL"
  last=$(ls -t "$sd"/*.bmp 2>/dev/null | head -1)
  if [ -n "$last" ]; then b=$(brightness "$last"); verdict=$([ "${b%.*}" -gt 3 ] 2>/dev/null && echo "RENDER(luma=$b)" || echo "BLACK(luma=$b)")
  else verdict="NO-FRAME(exit=$rc)"; fi
  printf "%-14s %s%s\n" "$name" "$verdict" "$crash"
done
echo
echo "frames + logs under: $OUT"
