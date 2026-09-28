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
  icuuc.dll
)

# OpenCV carries its version in the FILE NAME (opencv_world4130 -> 4140 when
# upstream bumped it in b8816a1e0), so a hardcoded name here rots the moment
# upstream updates the dependency -- and rots SILENTLY, because a previous
# build's DLL is still sitting in bin/ to satisfy the copy. Preview 4 was cut
# that way once: the zip shipped 4130 beside an exe linked against 4140 and
# every download died on startup with STATUS_DLL_NOT_FOUND (0xC0000135).
# Resolve it from the exe's own import table instead of naming it.
OPENCV="$(grep -aoE 'opencv_world[0-9]+\.dll' "$BIN/rpcs3.exe" | sort -u | head -1)"
[ -n "$OPENCV" ] || { echo "package-release: no opencv_world*.dll import in rpcs3.exe" >&2; exit 1; }
FILES+=("$OPENCV")

for f in "${FILES[@]}"; do
  cp -- "$BIN/$f" "$OUT/$f"
done

# The FFmpeg DLLs carry a soname number too (avcodec-61, swscale-8, ...) and rot
# the same way, but unlike OpenCV they are not named in rpcs3.exe's own strings
# -- Qt loads them -- so they cannot be resolved from the exe. Glob the family
# instead and refuse to guess when bin/ holds more than one version, which is
# exactly the stale-leftover situation that hid the OpenCV bump.
for fam in avcodec avformat avutil swresample swscale; do
  found=$(ls -1 "$BIN/$fam"-*.dll 2>/dev/null | wc -l)
  if [ "$found" -eq 0 ]; then
    echo "package-release: no $fam-*.dll in $BIN" >&2; exit 1
  elif [ "$found" -gt 1 ]; then
    echo "package-release: $found versions of $fam-*.dll in $BIN; delete the stale one:" >&2
    ls -1 "$BIN/$fam"-*.dll >&2; exit 1
  fi
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
