#!/usr/bin/env bash
# Assemble a distributable RPCS3-RTX build from bin/ using an explicit allowlist.
# Nothing is copied that is not named here -- bin/ contains firmware, games, saves,
# caches and the Remix runtime, none of which may ship.
set -euo pipefail

BIN="${1:?usage: package-release.sh <bin dir> <staging dir>}"
OUT="${2:?usage: package-release.sh <bin dir> <staging dir>}"
# Repo root, resolved from this script rather than the caller's cwd.
REPO="$(cd "$(dirname "$0")/.." && pwd)"

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

# --- per-game Remix profiles (<TITLEID>.conf) ----------------------------
# Read once at backend init, before any knob latches; see RemixGameConfig.h. These are the
# settled per-title configuration that used to live in a launcher script and therefore never
# reached anyone but the developer, which is most of what a build is worth on this fork.
for c in "$BIN"/[A-Z][A-Z][A-Z][A-Z][0-9][0-9][0-9][0-9][0-9].conf; do
  [ -e "$c" ] && cp -- "$c" "$OUT/$(basename "$c")"
done

# --- directories rpcs3 expects to exist ----------------------------------
for d in dev_bdvd dev_flash dev_flash2 dev_flash3 dev_hdd0 dev_hdd1 \
         dev_usb000 games savestates shaderlog sounds ppu_progs spu_progs; do
  mkdir -p "$OUT/$d"
done

# --- SETUP.txt ------------------------------------------------------------
# The zip's own instructions. This was hand-written and hand-added for preview 1, so the
# packaging step could not reproduce its own output: a second release cut with this script
# alone would have shipped without it. Templated now -- prose in tools/SETUP.txt.in, and only
# the volatile build-provenance line substituted here, so it cannot drift from the build it
# ships beside.
GIT_SHORT="$(git -C "$REPO" rev-parse --short=8 HEAD)"
GIT_REV="$(git -C "$REPO" describe --tags 2>/dev/null || echo "$GIT_SHORT")"
sed -e "s/@GIT_SHORT@/$GIT_SHORT/g" -e "s/@GIT_REV@/$GIT_REV/g" \
    "$REPO/tools/SETUP.txt.in" > "$OUT/SETUP.txt"

echo "staged: $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1)"

# --- the distributable zip ------------------------------------------------
# Everything lives under a single top-level rpcs3-rtx-remix/ folder, so extracting the zip
# cannot scatter hundreds of files across whatever directory the user was in. This wrapping
# was also done by hand for preview 1 and is now part of the script.
#
# Git Bash on Windows ships no `zip`, so 7-Zip is a fallback rather than a hard failure.
ZIP_NAME="rpcs3-rtx-remix-$GIT_SHORT-win64.zip"
ZIP_PATH="$(cd "$(dirname "$OUT")" && pwd)/$ZIP_NAME"
SEVENZIP=""
if command -v 7z >/dev/null 2>&1; then
  SEVENZIP="7z"
elif [ -x "/c/Program Files/7-Zip/7z.exe" ]; then
  SEVENZIP="/c/Program Files/7-Zip/7z.exe"
fi

if command -v zip >/dev/null 2>&1 || [ -n "$SEVENZIP" ]; then
  WRAP="$(dirname "$OUT")/.pkgwrap"
  rm -rf "$WRAP"
  mkdir -p "$WRAP"
  cp -r -- "$OUT" "$WRAP/rpcs3-rtx-remix"
  rm -f "$ZIP_PATH"
  if command -v zip >/dev/null 2>&1; then
    ( cd "$WRAP" && zip -qr "$ZIP_PATH" rpcs3-rtx-remix )
  else
    ( cd "$WRAP" && "$SEVENZIP" a -tzip -bso0 -bsp0 "$ZIP_PATH" rpcs3-rtx-remix )
  fi
  rm -rf "$WRAP"
  echo "zipped: $ZIP_PATH ($(du -h "$ZIP_PATH" | cut -f1))"
else
  echo "zip: SKIPPED -- neither 'zip' nor 7-Zip found. Staged tree is at $OUT;"
  echo "     archive it yourself with a single top-level rpcs3-rtx-remix/ folder."
fi
