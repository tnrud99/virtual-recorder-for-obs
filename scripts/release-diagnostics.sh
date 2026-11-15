#!/usr/bin/env bash
set -u -o pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

REPORT_DIR="${REPORT_DIR:-"$ROOT_DIR/release/diagnostics"}"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
REPORT_FILE="${REPORT_FILE:-"$REPORT_DIR/virtual-recorder-diagnostics-$TIMESTAMP.txt"}"
RUN_PREFLIGHT="${RUN_PREFLIGHT:-1}"
REQUIRE_RECORDING="${REQUIRE_RECORDING:-0}"
RUN_RECORDING_CHECK="${RUN_RECORDING_CHECK:-$REQUIRE_RECORDING}"
RUN_ARCHIVE_CHECK="${RUN_ARCHIVE_CHECK:-1}"

mkdir -p "$REPORT_DIR"
exec > >(tee "$REPORT_FILE") 2>&1

failures=0

run_step()
{
  local title="$1"
  shift

  echo
  echo "==> $title"
  if "$@"; then
    echo "PASS: $title"
  else
    local status=$?
    echo "FAIL ($status): $title"
    failures=$((failures + 1))
  fi
}

print_value()
{
  local label="$1"
  local value="$2"
  printf '%-22s %s\n' "$label:" "${value:-unknown}"
}

echo "Virtual Recorder release diagnostics"
print_value "Report" "$REPORT_FILE"
print_value "Generated" "$(date)"
print_value "Repository" "$ROOT_DIR"
print_value "Git commit" "$(git rev-parse --short HEAD 2>/dev/null || true)"
print_value "Git branch" "$(git branch --show-current 2>/dev/null || true)"
print_value "Project version" "$(sed -n 's/^project(virtuedit-obs VERSION \([^ )]*\).*/\1/p' CMakeLists.txt | head -n 1)"
print_value "Player version" "$(awk -F"'" '/PLAYER_VERSION =/ { print $2; exit }' web/obs-player.html)"

echo
echo "==> Git status"
git status --short || failures=$((failures + 1))

if [[ "$RUN_PREFLIGHT" == "1" ]]; then
  run_step "Release preflight" scripts/release-preflight.sh
else
  echo
  echo "==> Release preflight"
  echo "SKIP: RUN_PREFLIGHT=$RUN_PREFLIGHT"
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
  run_step "Installed macOS plugin verification" scripts/verify-macos-install.sh
else
  echo
  echo "==> Installed plugin verification"
  echo "SKIP: not macOS"
fi

run_step "OBS log verification" env REQUIRE_RECORDING="$REQUIRE_RECORDING" scripts/check-obs-log.sh

if [[ "$RUN_ARCHIVE_CHECK" == "1" ]]; then
  run_step "Release archive verification" scripts/verify-release-archives.sh
else
  echo
  echo "==> Release archive verification"
  echo "SKIP: RUN_ARCHIVE_CHECK=$RUN_ARCHIVE_CHECK"
fi

if [[ "$RUN_RECORDING_CHECK" == "1" ]]; then
  run_step "Latest recording artifact verification" scripts/check-latest-recording.sh
else
  echo
  echo "==> Latest recording artifact verification"
  echo "SKIP: set RUN_RECORDING_CHECK=1 or REQUIRE_RECORDING=1 after a smoke recording"
fi

echo
if [[ "$failures" -gt 0 ]]; then
  echo "Release diagnostics failed with $failures issue(s)."
  echo "Report saved to: $REPORT_FILE"
  exit 1
fi

echo "Release diagnostics passed."
echo "Report saved to: $REPORT_FILE"
