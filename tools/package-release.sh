#!/usr/bin/env bash
# Assemble a distributable RPCS3-RTX build from bin/ using an explicit allowlist.
# Nothing is copied that is not named here -- bin/ contains firmware, games, saves,
# caches and the Remix runtime, none of which may ship.
set -euo pipefail

BIN="${1:?usage: package-release.sh <bin dir> <staging dir>}"
OUT="${2:?usage: package-release.sh <bin dir> <staging dir>}"

rm -rf "$OUT"
mkdir -p "$OUT"

# --- executable and its runtime DLLs -------------------------------------
FILES=(
  rpcs3.exe
  Qt6Concurrent.dll Qt6Core.dll Qt6Gui.dll Qt6Multimedia.dll
  Qt6MultimediaWidgets.dll Qt6Network.dll Qt6Svg.dll Qt6SvgWidgets.dll
  Qt6Widgets.dll
  avcodec-61.dll avformat-61.dll avutil-59.dll
  swresample-5.dll swscale-8.dll
  icuuc.dll opencv_world4130.dll
)
for f in "${FILES[@]}"; do
  cp -- "$BIN/$f" "$OUT/$f"
done

# --- Qt plugins, icons, homebrew test elfs -------------------------------
cp -r -- "$BIN/qt6"   "$OUT/qt6"
cp -r -- "$BIN/Icons" "$OUT/Icons"
cp -r -- "$BIN/test"  "$OUT/test"

# --- GUI themes only; CurrentSettings.ini / persistent_settings.dat are
#     user state (window geometry, recent-games list) and must not ship.
mkdir -p "$OUT/GuiConfigs"
find "$BIN/GuiConfigs" -maxdepth 1 -type f \
     \( -name '*.qss' -o -name '*.jpg' -o -name '*.png' \) \
     -exec cp -- {} "$OUT/GuiConfigs/" \;
cp -r -- "$BIN/GuiConfigs/dark"  "$OUT/GuiConfigs/dark"
cp -r -- "$BIN/GuiConfigs/light" "$OUT/GuiConfigs/light"

# --- per-title configs: these are the tuned Remix settings the readme
#     describes, and are the whole point of shipping a build.
mkdir -p "$OUT/config/custom_configs"
cp -- "$BIN/config/custom_configs/"config_*.yml "$OUT/config/custom_configs/"

# --- directories rpcs3 expects to exist ----------------------------------
for d in dev_bdvd dev_flash dev_flash2 dev_flash3 dev_hdd0 dev_hdd1 \
         dev_usb000 games savestates shaderlog sounds ppu_progs spu_progs; do
  mkdir -p "$OUT/$d"
done

echo "staged: $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1)"
