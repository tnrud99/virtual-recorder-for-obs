#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "==> Checking shell scripts"
bash -n \
  scripts/build-macos.sh \
  scripts/install-macos-dev.sh \
  scripts/build-ffmpeg-lgpl.sh \
  scripts/package-macos.sh \
  scripts/verify-macos-install.sh \
  scripts/check-obs-log.sh \
  scripts/check-latest-recording.sh \
  scripts/verify-release-archives.sh \
  scripts/download-latest-windows-artifact.sh \
  scripts/release-diagnostics.sh \
  scripts/release-preflight.sh

echo "==> Checking version consistency"
cmake_version="$(sed -nE 's/^project\(virtuedit-obs VERSION ([0-9.]+)\).*/\1/p' CMakeLists.txt)"
spec_version="$(sed -nE 's/^[[:space:]]*"version": "([0-9.]+)",?$/\1/p' buildspec.json | head -1)"
if [[ -z "$cmake_version" || "$cmake_version" != "$spec_version" ]]; then
  echo "ERROR: version mismatch: CMakeLists.txt=$cmake_version buildspec.json=$spec_version" >&2
  exit 1
fi
echo "Version $cmake_version"

echo "==> Building plugin"
cmake --build build

echo "==> Running C++ tests"
ctest --test-dir build --output-on-failure

echo "==> Validating sync JSON fixtures"
node tests/validate-obs-sync-json.mjs tests/fixtures/*.json

echo "==> Checking hosted OBS player mirror"
VIEWER_PLAYER="${VIEWER_PLAYER:-"$ROOT_DIR/../viewer/src/server/public/obs-player.html"}"
if [[ ! -f "$VIEWER_PLAYER" ]]; then
  echo "ERROR: viewer obs-player.html not found: $VIEWER_PLAYER"
  exit 1
fi

cmp -s web/obs-player.html "$VIEWER_PLAYER" || {
  echo "ERROR: web/obs-player.html differs from viewer deployed source:"
  echo "  $VIEWER_PLAYER"
  exit 1
}

PLUGIN_VERSION="$(awk -F'"' '/PLAYER_VERSION =/ { print $2; exit }' src/youtube-controller.cpp)"
PLAYER_VERSION="$(awk -F"'" '/PLAYER_VERSION =/ { print $2; exit }' web/obs-player.html)"
if [[ -z "$PLUGIN_VERSION" || -z "$PLAYER_VERSION" || "$PLUGIN_VERSION" != "$PLAYER_VERSION" ]]; then
  echo "ERROR: obs-player version mismatch"
  echo "  plugin: ${PLUGIN_VERSION:-missing}"
  echo "  player: ${PLAYER_VERSION:-missing}"
  exit 1
fi

if [[ "${CHECK_DEPLOYED_PLAYER:-1}" == "1" ]]; then
  TMP_PLAYER="$(mktemp)"
  trap 'rm -f "$TMP_PLAYER"' EXIT
  curl -fsSL https://virtuedit.com/obs-player.html -o "$TMP_PLAYER"
  cmp -s web/obs-player.html "$TMP_PLAYER" || {
    echo "ERROR: deployed https://virtuedit.com/obs-player.html differs from local web/obs-player.html"
    exit 1
  }
fi

echo "==> Checking diff whitespace"
git diff --check

echo "Release preflight passed."
