#!/usr/bin/env bash
set -euo pipefail

LOG_DIR="${OBS_LOG_DIR:-"$HOME/Library/Application Support/obs-studio/logs"}"
LOG_FILE="${OBS_LOG:-}"
REQUIRE_RECORDING="${REQUIRE_RECORDING:-0}"

if [[ -z "$LOG_FILE" ]]; then
  LOG_FILE="$(ls -t "$LOG_DIR"/*.txt 2>/dev/null | head -n 1 || true)"
fi

if [[ -z "$LOG_FILE" || ! -f "$LOG_FILE" ]]; then
  echo "ERROR: OBS log file not found. Set OBS_LOG=/path/to/log.txt if needed." >&2
  exit 1
fi

echo "==> Checking OBS log"
echo "Log:"
echo "  $LOG_FILE"

failures=0

fail()
{
  failures=$((failures + 1))
  echo "ERROR: $*" >&2
}

require_pattern()
{
  local pattern="$1"
  local message="$2"
  if ! grep -Eq "$pattern" "$LOG_FILE"; then
    fail "$message"
  fi
}

reject_pattern()
{
  local pattern="$1"
  local message="$2"
  if grep -En "$pattern" "$LOG_FILE"; then
    fail "$message"
  fi
}

require_pattern "\\[VirtuEdit\\] Plugin loaded" \
  "Virtual Recorder plugin load message was not found"
require_pattern "\\[VirtuEdit\\] WebSocket server started" \
  "Virtual Recorder WebSocket server start message was not found"

reject_pattern "\\[VirtuEdit\\].*(Failed|failed|Recording aborted|version mismatch|invalid session token|ffmpeg not found|Using system ffmpeg|Bundled LGPL ffmpeg was not found|remux failed|Composite recording path was not reported)" \
  "Virtual Recorder failure/warning pattern found in OBS log"

if [[ "$REQUIRE_RECORDING" == "1" ]]; then
  require_pattern "\\[VirtuEdit\\] Recording started" \
    "Recording started message was not found"
  require_pattern "\\[VirtuEdit\\] Recording stopped" \
    "Recording stopped message was not found"
  require_pattern "\\[VirtuEdit\\] JSON saved:" \
    "Sync JSON saved message was not found"
  require_pattern "\\[VirtuEdit\\] Timestamp producer selected:" \
    "Timestamp producer selection message was not found"
fi

if [[ "$failures" -gt 0 ]]; then
  echo "OBS log verification failed with $failures issue(s)." >&2
  exit 1
fi

echo "OBS log verification passed."
