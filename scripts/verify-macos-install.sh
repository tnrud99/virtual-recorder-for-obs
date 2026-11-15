#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN_BUNDLE="${PLUGIN_BUNDLE:-"$HOME/Library/Application Support/obs-studio/plugins/virtuedit-obs.plugin"}"
PLUGIN_BINARY="$PLUGIN_BUNDLE/Contents/MacOS/virtuedit-obs"
FFMPEG_BINARY="$PLUGIN_BUNDLE/Contents/Resources/ffmpeg"
INFO_PLIST="$PLUGIN_BUNDLE/Contents/Info.plist"

fail()
{
  echo "ERROR: $*" >&2
  exit 1
}

echo "==> Checking macOS plugin install"
echo "Plugin bundle:"
echo "  $PLUGIN_BUNDLE"

[[ -d "$PLUGIN_BUNDLE" ]] || fail "plugin bundle not found"
[[ -x "$PLUGIN_BINARY" ]] || fail "plugin binary missing or not executable: $PLUGIN_BINARY"
[[ -f "$INFO_PLIST" ]] || fail "Info.plist missing: $INFO_PLIST"
[[ -d "$PLUGIN_BUNDLE/Contents/Resources/locale" ]] || fail "locale directory missing"
[[ -f "$PLUGIN_BUNDLE/Contents/Resources/locale/en-US.ini" ]] || fail "en-US locale missing"
[[ -f "$PLUGIN_BUNDLE/Contents/Resources/locale/ko-KR.ini" ]] || fail "ko-KR locale missing"
[[ -f "$PLUGIN_BUNDLE/Contents/Resources/LICENSE.txt" ]] || fail "GPL license missing"
[[ -f "$PLUGIN_BUNDLE/Contents/Resources/THIRD_PARTY_LICENSES.txt" ]] || fail "third-party notices missing"
[[ -f "$PLUGIN_BUNDLE/Contents/Resources/SOURCE_CODE.txt" ]] || fail "source information missing"
[[ -f "$PLUGIN_BUNDLE/Contents/Resources/licenses/LGPL-2.1.txt" ]] || fail "LGPL-2.1 license missing"
[[ -f "$PLUGIN_BUNDLE/Contents/Resources/licenses/LGPL-3.0.txt" ]] || fail "LGPL-3.0 license missing"
[[ -x "$FFMPEG_BINARY" ]] || fail "bundled ffmpeg missing or not executable: $FFMPEG_BINARY"

if "$FFMPEG_BINARY" -version 2>/dev/null | grep -q -- "--enable-gpl"; then
  fail "bundled ffmpeg was built with --enable-gpl"
fi

if command -v plutil >/dev/null 2>&1; then
  plutil -lint "$INFO_PLIST" >/dev/null || fail "Info.plist is invalid"
fi

PLUGIN_VERSION="$(sed -n 's/^project(virtuedit-obs VERSION \([^ )]*\).*/\1/p' "$ROOT_DIR/CMakeLists.txt" | head -n 1)"
INSTALLED_VERSION=""
if command -v plutil >/dev/null 2>&1; then
  INSTALLED_VERSION="$(plutil -extract CFBundleShortVersionString raw "$INFO_PLIST" 2>/dev/null || true)"
fi
if [[ -n "$PLUGIN_VERSION" && -n "$INSTALLED_VERSION" && "$PLUGIN_VERSION" != "$INSTALLED_VERSION" ]]; then
  fail "installed version mismatch: installed=$INSTALLED_VERSION expected=$PLUGIN_VERSION"
fi

echo "macOS plugin install verification passed."
