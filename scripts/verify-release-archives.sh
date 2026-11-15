#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHECK_MAC_ZIP="${CHECK_MAC_ZIP:-auto}"
CHECK_WINDOWS_ZIP="${CHECK_WINDOWS_ZIP:-auto}"

find_latest_zip()
{
  local name="$1"
  local candidates=()
  local path

  if [[ ! -d "$ROOT_DIR/release" ]]; then
    return
  fi

  while IFS= read -r -d '' path; do
    candidates+=("$path")
  done < <(find "$ROOT_DIR/release" -type f -name "$name" -print0 2>/dev/null)

  if [[ "${#candidates[@]}" -gt 0 ]]; then
    ls -t "${candidates[@]}" | head -n 1
  fi
}

MAC_ZIP="${MAC_ZIP:-"$(find_latest_zip "VirtuEdit-OBS-Plugin-macOS-arm64.zip")"}"
WINDOWS_ZIP="${WINDOWS_ZIP:-"$(find_latest_zip "VirtuEdit-OBS-Plugin-Windows-x64.zip")"}"

failures=0

fail()
{
  failures=$((failures + 1))
  echo "ERROR: $*" >&2
}

should_check()
{
  local mode="$1"
  local path="$2"
  case "$mode" in
    1|true|TRUE|yes|YES)
      return 0
      ;;
    0|false|FALSE|no|NO)
      return 1
      ;;
    auto)
      [[ -f "$path" ]]
      return
      ;;
    *)
      fail "invalid check mode: $mode"
      return 1
      ;;
  esac
}

zip_has()
{
  local list_file="$1"
  local entry="$2"
  grep -Fxq "$entry" "$list_file"
}

require_zip_entry()
{
  local list_file="$1"
  local entry="$2"
  local label="$3"
  if ! zip_has "$list_file" "$entry"; then
    fail "$label is missing: $entry"
  fi
}

forbid_zip_entry()
{
  local list_file="$1"
  local entry="$2"
  local label="$3"
  if zip_has "$list_file" "$entry"; then
    fail "$label should not be packaged: $entry"
  fi
}

check_no_metadata()
{
  local list_file="$1"
  local label="$2"
  if grep -Eq '(^|/)__MACOSX(/|$)|(^|/)\._' "$list_file"; then
    fail "$label contains macOS metadata files"
  fi
}

check_macos_zip()
{
  local zip_path="$1"
  echo
  echo "==> Checking macOS release ZIP"
  echo "ZIP:"
  echo "  $zip_path"

  [[ -f "$zip_path" ]] || {
    fail "macOS ZIP not found: $zip_path"
    return
  }

  local list_file
  list_file="$(mktemp)"
  unzip -Z1 "$zip_path" >"$list_file"

  require_zip_entry "$list_file" "virtuedit-obs.plugin/Contents/MacOS/virtuedit-obs" "macOS plugin binary"
  require_zip_entry "$list_file" "virtuedit-obs.plugin/Contents/Resources/ffmpeg" "macOS bundled ffmpeg"
  require_zip_entry "$list_file" "virtuedit-obs.plugin/Contents/Resources/locale/en-US.ini" "macOS en-US locale"
  require_zip_entry "$list_file" "virtuedit-obs.plugin/Contents/Resources/locale/ko-KR.ini" "macOS ko-KR locale"
  forbid_zip_entry "$list_file" "virtuedit-obs.plugin/Contents/Resources/obs-player.html" "macOS hosted player copy (the plugin loads the player from virtuedit.com)"
  require_zip_entry "$list_file" "virtuedit-obs.plugin/Contents/Resources/LICENSE.txt" "macOS GPL license"
  require_zip_entry "$list_file" "virtuedit-obs.plugin/Contents/Resources/THIRD_PARTY_LICENSES.txt" "macOS third-party notices"
  require_zip_entry "$list_file" "virtuedit-obs.plugin/Contents/Resources/SOURCE_CODE.txt" "macOS source information"
  require_zip_entry "$list_file" "virtuedit-obs.plugin/Contents/Resources/licenses/LGPL-2.1.txt" "macOS LGPL-2.1 license"
  require_zip_entry "$list_file" "virtuedit-obs.plugin/Contents/Resources/licenses/LGPL-3.0.txt" "macOS LGPL-3.0 license"
  require_zip_entry "$list_file" "virtuedit-obs.plugin/Contents/Info.plist" "macOS Info.plist"
  check_no_metadata "$list_file" "macOS ZIP"

  rm -f "$list_file"
}

check_windows_zip()
{
  local zip_path="$1"
  echo
  echo "==> Checking Windows release ZIP"
  echo "ZIP:"
  echo "  $zip_path"

  [[ -f "$zip_path" ]] || {
    fail "Windows ZIP not found: $zip_path"
    return
  }

  local list_file
  list_file="$(mktemp)"
  unzip -Z1 "$zip_path" >"$list_file"

  require_zip_entry "$list_file" "Install-Windows.bat" "Windows installer"
  require_zip_entry "$list_file" "Uninstall-Windows.bat" "Windows uninstaller"
  require_zip_entry "$list_file" "How to Install.txt" "Windows install guide"
  require_zip_entry "$list_file" "virtuedit-obs/bin/64bit/virtuedit-obs.dll" "Windows plugin DLL"
  require_zip_entry "$list_file" "virtuedit-obs/data/ffmpeg.exe" "Windows bundled ffmpeg"
  require_zip_entry "$list_file" "virtuedit-obs/data/locale/en-US.ini" "Windows en-US locale"
  require_zip_entry "$list_file" "virtuedit-obs/data/locale/ko-KR.ini" "Windows ko-KR locale"
  forbid_zip_entry "$list_file" "virtuedit-obs/INSTALL.txt" "duplicate install guide (use How to Install.txt)"
  require_zip_entry "$list_file" "virtuedit-obs/LICENSE.txt" "Windows license"
  require_zip_entry "$list_file" "virtuedit-obs/THIRD_PARTY_LICENSES.txt" "Windows third-party licenses"
  require_zip_entry "$list_file" "virtuedit-obs/SOURCE_CODE.txt" "Windows source information"
  require_zip_entry "$list_file" "virtuedit-obs/licenses/LGPL-2.1.txt" "Windows LGPL-2.1 license"
  require_zip_entry "$list_file" "virtuedit-obs/licenses/LGPL-3.0.txt" "Windows LGPL-3.0 license"
  forbid_zip_entry "$list_file" "INSTALL.txt" "old root install guide"
  forbid_zip_entry "$list_file" "Verify-Windows-Install.ps1" "manual verification script"
  check_no_metadata "$list_file" "Windows ZIP"

  rm -f "$list_file"
}

if should_check "$CHECK_MAC_ZIP" "$MAC_ZIP"; then
  check_macos_zip "$MAC_ZIP"
else
  echo "SKIP: macOS ZIP verification"
fi

if should_check "$CHECK_WINDOWS_ZIP" "$WINDOWS_ZIP"; then
  check_windows_zip "$WINDOWS_ZIP"
else
  echo "SKIP: Windows ZIP verification"
fi

if [[ "$failures" -gt 0 ]]; then
  echo
  echo "Release archive verification failed with $failures issue(s)." >&2
  exit 1
fi

echo
echo "Release archive verification passed."
