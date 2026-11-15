#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MACOS_ARCHITECTURE="${MACOS_ARCHITECTURE:-arm64}"
case "$MACOS_ARCHITECTURE" in
  arm64)
    BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
    CUSTOM_FFMPEG="$ROOT_DIR/build/ffmpeg-dist/bin/ffmpeg"
    ;;
  x86_64)
    BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build/macos-x86_64"}"
    CUSTOM_FFMPEG="$ROOT_DIR/build/ffmpeg-dist-x86_64/bin/ffmpeg"
    ;;
  *) echo "Unsupported Mac architecture: $MACOS_ARCHITECTURE" >&2; exit 1 ;;
esac
PLUGIN_BUNDLE="${PLUGIN_BUNDLE:-"$HOME/Library/Application Support/obs-studio/plugins/virtuedit-obs.plugin"}"

if [[ "${SKIP_OBS_CHECK:-0}" != "1" ]] && pgrep -x OBS >/dev/null 2>&1; then
  echo "OBS Studio is running. Quit OBS before installing the plugin bundle."
  exit 1
fi

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
  MACOS_ARCHITECTURE="$MACOS_ARCHITECTURE" BUILD_DIR="$BUILD_DIR" "$ROOT_DIR/scripts/build-macos.sh"
fi

if [[ ! -f "$BUILD_DIR/virtuedit-obs.so" ]]; then
  echo "Plugin binary not found at $BUILD_DIR/virtuedit-obs.so"
  echo "Run scripts/build-macos.sh first, or unset SKIP_BUILD."
  exit 1
fi

if [[ "$(lipo -archs "$BUILD_DIR/virtuedit-obs.so")" != "$MACOS_ARCHITECTURE" ]]; then
  echo "Plugin architecture does not match $MACOS_ARCHITECTURE." >&2
  exit 1
fi

rm -rf "$PLUGIN_BUNDLE"
mkdir -p "$PLUGIN_BUNDLE/Contents/MacOS"
mkdir -p "$PLUGIN_BUNDLE/Contents/Resources/locale"

cp "$BUILD_DIR/virtuedit-obs.so" \
  "$PLUGIN_BUNDLE/Contents/MacOS/virtuedit-obs"
chmod +x "$PLUGIN_BUNDLE/Contents/MacOS/virtuedit-obs"

# OBS supplies Qt at runtime. Keep plugin dependency names compatible with
# OBS.app even when a developer uses a non-OBS Qt installation locally.
for framework in QtWidgets QtGui QtCore QtNetwork; do
  dep="$(otool -L "$PLUGIN_BUNDLE/Contents/MacOS/virtuedit-obs" \
    | awk '{print $1}' \
    | grep "/${framework}.framework/Versions/A/${framework}" \
    | head -n 1 || true)"
  if [[ -n "$dep" && "$dep" != @rpath/* ]]; then
    install_name_tool -change "$dep" \
      "@rpath/${framework}.framework/Versions/A/${framework}" \
      "$PLUGIN_BUNDLE/Contents/MacOS/virtuedit-obs"
  fi
done

cp "$ROOT_DIR"/data/locale/*.ini \
  "$PLUGIN_BUNDLE/Contents/Resources/locale/"

# Ship license and corresponding-source information inside the signed bundle.
mkdir -p "$PLUGIN_BUNDLE/Contents/Resources/licenses"
cp "$ROOT_DIR/LICENSE" \
  "$PLUGIN_BUNDLE/Contents/Resources/LICENSE.txt"
cp "$ROOT_DIR/THIRD_PARTY_LICENSES" \
  "$PLUGIN_BUNDLE/Contents/Resources/THIRD_PARTY_LICENSES.txt"
cp "$ROOT_DIR/SOURCE_CODE.md" \
  "$PLUGIN_BUNDLE/Contents/Resources/SOURCE_CODE.txt"
cp "$ROOT_DIR/licenses/LGPL-2.1.txt" \
  "$PLUGIN_BUNDLE/Contents/Resources/licenses/LGPL-2.1.txt"
cp "$ROOT_DIR/licenses/LGPL-3.0.txt" \
  "$PLUGIN_BUNDLE/Contents/Resources/licenses/LGPL-3.0.txt"

# Bundle ffmpeg binary (plugin searches this location first).
# Prefer the custom-built LGPL binary (scripts/build-ffmpeg-lgpl.sh).
if [[ -f "$CUSTOM_FFMPEG" ]]; then
  FFMPEG_BIN="$CUSTOM_FFMPEG"
  echo "  Using LGPL ffmpeg: $FFMPEG_BIN"
else
  if [[ "${REQUIRE_LGPL_FFMPEG:-0}" == "1" ]]; then
    echo "  ERROR: LGPL ffmpeg not found at $CUSTOM_FFMPEG"
    echo "         Run scripts/build-ffmpeg-lgpl.sh before release packaging."
    exit 1
  fi
  FFMPEG_BIN="$(command -v ffmpeg 2>/dev/null || true)"
  if [[ -z "$FFMPEG_BIN" ]]; then
    for p in /opt/homebrew/bin/ffmpeg /usr/local/bin/ffmpeg; do
      [[ -f "$p" ]] && FFMPEG_BIN="$p" && break
    done
  fi
  if [[ -n "$FFMPEG_BIN" ]]; then
    echo "  WARNING: LGPL ffmpeg not found, falling back to system ffmpeg."
    echo "           Run scripts/build-ffmpeg-lgpl.sh first for a clean LGPL build."
  fi
fi
if [[ -n "${FFMPEG_BIN:-}" ]]; then
  if ! lipo "$FFMPEG_BIN" -verify_arch "$MACOS_ARCHITECTURE"; then
    echo "Bundled FFmpeg does not support $MACOS_ARCHITECTURE." >&2
    exit 1
  fi
  ffmpeg_version="$("$FFMPEG_BIN" -version)"
  if [[ "$ffmpeg_version" == *--enable-gpl* ]]; then
    if [[ "${REQUIRE_LGPL_FFMPEG:-0}" == "1" ]]; then
      echo "  ERROR: Selected ffmpeg was built with --enable-gpl: $FFMPEG_BIN"
      echo "         Use scripts/build-ffmpeg-lgpl.sh for release packaging."
      exit 1
    fi
    echo "  WARNING: selected ffmpeg was built with --enable-gpl."
    echo "           Do not ship this binary in a release package."
  fi
  cp "$FFMPEG_BIN" "$PLUGIN_BUNDLE/Contents/Resources/ffmpeg"
  chmod +x "$PLUGIN_BUNDLE/Contents/Resources/ffmpeg"
  # Ad-hoc sign for Gatekeeper
  codesign --force --sign - "$PLUGIN_BUNDLE/Contents/Resources/ffmpeg" 2>/dev/null || true
  echo "  Bundled ffmpeg from: $FFMPEG_BIN"
else
  echo "  WARNING: ffmpeg not found -- plugin will try system PATH at runtime"
fi

# Extract version from CMakeLists.txt (single source of truth)
PLUGIN_VERSION="$(sed -n 's/^project(virtuedit-obs VERSION \([^ )]*\).*/\1/p' "$ROOT_DIR/CMakeLists.txt")"
PLUGIN_VERSION="${PLUGIN_VERSION:-1.0.0}"

cat > "$PLUGIN_BUNDLE/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>virtuedit-obs</string>
  <key>CFBundleIdentifier</key>
  <string>com.virtuedit.obs-plugin</string>
  <key>CFBundleName</key>
  <string>virtuedit-obs</string>
  <key>CFBundlePackageType</key>
  <string>BNDL</string>
  <key>CFBundleVersion</key>
  <string>${PLUGIN_VERSION}</string>
  <key>CFBundleShortVersionString</key>
  <string>${PLUGIN_VERSION}</string>
</dict>
</plist>
PLIST

if codesign --force --deep --sign - "$PLUGIN_BUNDLE" 2>/dev/null; then
  echo "  Ad-hoc signed plugin bundle"
else
  if [[ "${REQUIRE_LGPL_FFMPEG:-0}" == "1" ]]; then
    echo "  ERROR: Failed to ad-hoc sign plugin bundle."
    exit 1
  fi
  echo "  WARNING: Failed to ad-hoc sign plugin bundle."
fi

echo "Installed VirtuEdit OBS plugin bundle to:"
echo "  $PLUGIN_BUNDLE"
echo
echo "Restart OBS Studio. Virtual Recorder opens as its own dock."
echo "If it is hidden, open it from Tools > Open Virtual Recorder."
