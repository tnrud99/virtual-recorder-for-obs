#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OBS_VERSION="${OBS_VERSION:-32.1.2}"
OBS_SOURCE_DIR="${OBS_SOURCE_DIR:-"/tmp/obs-studio-$OBS_VERSION"}"
OBS_LIB_DIR="${OBS_LIB_DIR:-"/Applications/OBS.app/Contents/Frameworks"}"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-12.0}"
MACOS_ARCHITECTURE="${MACOS_ARCHITECTURE:-arm64}"
case "$MACOS_ARCHITECTURE" in
  arm64) BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}" ;;
  x86_64) BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build/macos-x86_64"}" ;;
  *) echo "Unsupported Mac architecture: $MACOS_ARCHITECTURE" >&2; exit 1 ;;
esac

if [[ -z "${QT_PREFIX:-}" ]]; then
  if [[ -n "${QT_ROOT_DIR:-}" ]]; then
    QT_PREFIX="$QT_ROOT_DIR"
  else
    QT_PREFIX="$(brew --prefix qt 2>/dev/null || brew --prefix qt@6 2>/dev/null || true)"
  fi
fi

if [[ -z "${QT_PREFIX:-}" || ! -d "$QT_PREFIX" ]]; then
  echo "Qt prefix not found. Install Qt 6.8.3 or set QT_PREFIX/QT_ROOT_DIR."
  exit 1
fi

if [[ ! -f "$OBS_SOURCE_DIR/libobs/obs-module.h" ]]; then
  echo "OBS source headers not found at $OBS_SOURCE_DIR"
  echo "Run:"
  echo "  git clone --depth 1 --branch $OBS_VERSION https://github.com/obsproject/obs-studio.git $OBS_SOURCE_DIR"
  exit 1
fi

if ! lipo "$OBS_LIB_DIR/libobs.framework/Versions/A/libobs" -verify_arch "$MACOS_ARCHITECTURE"; then
  echo "Set OBS_LIB_DIR to OBS Studio frameworks for $MACOS_ARCHITECTURE." >&2
  exit 1
fi

if ! brew list simde >/dev/null 2>&1; then
  echo "Missing simde headers. Run: brew install simde"
  exit 1
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="$MACOS_ARCHITECTURE" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
  -DOBS_SOURCE_DIR="$OBS_SOURCE_DIR" \
  -DOBS_LIB_DIR="$OBS_LIB_DIR"

cmake --build "$BUILD_DIR" --config Release

bad_qt_symbols="$(nm -u "$BUILD_DIR/virtuedit-obs.so" | grep -E 'QAnyStringView|QString6assign|arg_impl|toStdString' || true)"
if [[ -n "$bad_qt_symbols" ]]; then
  echo "Build produced Qt symbols that are not compatible with OBS Studio's bundled Qt:"
  echo "$bad_qt_symbols"
  echo
  echo "Build against the OBS-matched Qt dependency package."
  exit 1
fi

echo "Built: $BUILD_DIR/virtuedit-obs.so"
