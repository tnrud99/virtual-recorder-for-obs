#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_DIR="${VIRTUAL_RECORDER_DIR:-"$HOME/Movies/Virtual Recorder"}"
SESSION_DIR="${SESSION_DIR:-}"

fail()
{
  echo "ERROR: $*" >&2
  exit 1
}

if [[ -z "$SESSION_DIR" ]]; then
  [[ -d "$BASE_DIR" ]] || fail "recording folder not found: $BASE_DIR"
  shopt -s nullglob
  session_dirs=()
  for dir in "$BASE_DIR"/*/; do
    sync_candidates=("$dir"/*_sync.json)
    if [[ "${#sync_candidates[@]}" -gt 0 ]]; then
      session_dirs+=("$dir")
    fi
  done
  shopt -u nullglob
  [[ "${#session_dirs[@]}" -gt 0 ]] || fail "no Virtual Recorder session folder with *_sync.json found"
  SESSION_DIR="$(ls -td "${session_dirs[@]}" | head -n 1 || true)"
  SESSION_DIR="${SESSION_DIR%/}"
fi

[[ -n "$SESSION_DIR" ]] || fail "no Virtual Recorder session folder found"
[[ -d "$SESSION_DIR" ]] || fail "session folder not found: $SESSION_DIR"

echo "==> Checking latest Virtual Recorder session"
echo "Session:"
echo "  $SESSION_DIR"

shopt -s nullglob
reaction_files=("$SESSION_DIR"/*_reaction.mp4 "$SESSION_DIR"/*_reaction.mov "$SESSION_DIR"/*_reaction.mkv)
sync_files=("$SESSION_DIR"/*_sync.json)
combined_files=("$SESSION_DIR"/*_combined.*)
shopt -u nullglob

[[ "${#reaction_files[@]}" -eq 1 ]] || fail "expected exactly one reaction file, found ${#reaction_files[@]}"
[[ "${#sync_files[@]}" -eq 1 ]] || fail "expected exactly one sync JSON file, found ${#sync_files[@]}"
[[ "${#combined_files[@]}" -eq 1 ]] || fail "expected exactly one combined file, found ${#combined_files[@]}"

for file in "${reaction_files[0]}" "${sync_files[0]}" "${combined_files[0]}"; do
  [[ -f "$file" ]] || fail "missing file: $file"
  [[ -s "$file" ]] || fail "zero-byte file: $file"
done

node "$ROOT_DIR/tests/validate-obs-sync-json.mjs" "${sync_files[0]}"

echo "Reaction:"
echo "  ${reaction_files[0]}"
echo "Sync:"
echo "  ${sync_files[0]}"
echo "Combined:"
echo "  ${combined_files[0]}"
echo "Recording artifact verification passed."
