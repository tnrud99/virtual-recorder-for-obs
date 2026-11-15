#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE_ROOT="${PACKAGE_ROOT:-"$ROOT_DIR/release/macos"}"
MACOS_ARCHITECTURE="${MACOS_ARCHITECTURE:-arm64}"
stage_name="stage"
if [[ "$MACOS_ARCHITECTURE" == "x86_64" ]]; then
  stage_name="stage-x86_64"
fi
STAGE_DIR="${STAGE_DIR:-"$PACKAGE_ROOT/$stage_name"}"
ZIP_PATH="${ZIP_PATH:-"$PACKAGE_ROOT/VirtuEdit-OBS-Plugin-macOS-$MACOS_ARCHITECTURE.zip"}"
PLUGIN_BUNDLE="$STAGE_DIR/virtuedit-obs.plugin"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-12.0}"

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR" "$PACKAGE_ROOT"

if [[ "${BUILD_FFMPEG:-0}" == "1" ]]; then
  MACOS_ARCHITECTURE="$MACOS_ARCHITECTURE" "$ROOT_DIR/scripts/build-ffmpeg-lgpl.sh"
fi

REQUIRE_LGPL_FFMPEG=1 \
  SKIP_OBS_CHECK=1 \
  MACOS_ARCHITECTURE="$MACOS_ARCHITECTURE" \
  PLUGIN_BUNDLE="$PLUGIN_BUNDLE" \
  "$ROOT_DIR/scripts/install-macos-dev.sh"

PLUGIN_BINARY="$PLUGIN_BUNDLE/Contents/MacOS/virtuedit-obs"
FFMPEG_BINARY="$PLUGIN_BUNDLE/Contents/Resources/ffmpeg"

if [[ ! -x "$PLUGIN_BINARY" ]]; then
  echo "ERROR: plugin binary missing or not executable: $PLUGIN_BINARY"
  exit 1
fi

if [[ ! -x "$FFMPEG_BINARY" ]]; then
  echo "ERROR: bundled ffmpeg missing or not executable: $FFMPEG_BINARY"
  exit 1
fi

verify_macos_binary()
{
  local binary="$1"
  local label="$2"
  local architectures
  local minos

  architectures="$(lipo -archs "$binary")"
  [[ "$architectures" == "$MACOS_ARCHITECTURE" ]] || {
    echo "ERROR: $label must be $MACOS_ARCHITECTURE, found: $architectures"
    exit 1
  }

  minos="$(vtool -show-build "$binary" 2>/dev/null | awk '/minos/ { print $2; exit }')"
  [[ "$minos" == "$MACOS_DEPLOYMENT_TARGET" ]] || {
    echo "ERROR: $label minimum macOS is ${minos:-unknown}; expected $MACOS_DEPLOYMENT_TARGET"
    exit 1
  }

  if otool -L "$binary" | awk 'NR > 1 { print $1 }' \
      | grep -Eq '^(/opt/homebrew|/usr/local)/'; then
    echo "ERROR: $label depends on a local Homebrew library:"
    otool -L "$binary" | awk 'NR > 1 { print $1 }' \
      | grep -E '^(/opt/homebrew|/usr/local)/'
    exit 1
  fi
}

verify_macos_binary "$PLUGIN_BINARY" "plugin binary"
verify_macos_binary "$FFMPEG_BINARY" "bundled ffmpeg"

rm -f "$ZIP_PATH"
(cd "$STAGE_DIR" && COPYFILE_DISABLE=1 zip -qry "$ZIP_PATH" "virtuedit-obs.plugin")

if unzip -l "$ZIP_PATH" | grep -E '(^|/)\._' >/dev/null; then
  echo "ERROR: macOS ZIP contains AppleDouble metadata files."
  exit 1
fi

echo "Packaged macOS plugin ZIP:"
echo "  $ZIP_PATH"
