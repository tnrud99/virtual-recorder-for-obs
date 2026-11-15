#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKFLOW="${WORKFLOW:-Build OBS Plugin}"
BRANCH="${BRANCH:-main}"
ARTIFACT_NAME="${ARTIFACT_NAME:-VirtuEdit-OBS-Plugin-Windows-x64}"
RUN_ID="${RUN_ID:-}"
VERIFY_ARCHIVE="${VERIFY_ARCHIVE:-1}"

fail()
{
  echo "ERROR: $*" >&2
  exit 1
}

command -v gh >/dev/null 2>&1 || fail "GitHub CLI is required: gh"

if [[ -z "$RUN_ID" ]]; then
  RUN_ID="$(
    gh run list \
      --workflow "$WORKFLOW" \
      --branch "$BRANCH" \
      --status success \
      --limit 1 \
      --json databaseId \
      --jq '.[0].databaseId'
  )"
fi

[[ -n "$RUN_ID" && "$RUN_ID" != "null" ]] || fail "no successful GitHub Actions run found"

OUT_DIR="${OUT_DIR:-"$ROOT_DIR/release/github-actions-$RUN_ID"}"
ZIP_PATH="$OUT_DIR/VirtuEdit-OBS-Plugin-Windows-x64.zip"

echo "==> Downloading Windows CI artifact"
echo "Workflow:"
echo "  $WORKFLOW"
echo "Branch:"
echo "  $BRANCH"
echo "Run:"
echo "  $RUN_ID"
echo "Artifact:"
echo "  $ARTIFACT_NAME"
echo "Output:"
echo "  $OUT_DIR"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
gh run download "$RUN_ID" --name "$ARTIFACT_NAME" --dir "$OUT_DIR"

[[ -f "$ZIP_PATH" ]] || fail "downloaded artifact did not contain $ZIP_PATH"

if [[ "$VERIFY_ARCHIVE" == "1" ]]; then
  CHECK_MAC_ZIP=0 WINDOWS_ZIP="$ZIP_PATH" "$ROOT_DIR/scripts/verify-release-archives.sh"
fi

echo "Downloaded Windows release ZIP:"
echo "  $ZIP_PATH"
