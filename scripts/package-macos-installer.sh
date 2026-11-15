#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${OUTPUT_DIR:-"$ROOT_DIR/release/macos"}"
MACOS_ARCHITECTURE="${MACOS_ARCHITECTURE:-arm64}"
case "$MACOS_ARCHITECTURE" in
  arm64|x86_64) ;;
  *) echo "Unsupported Mac architecture: $MACOS_ARCHITECTURE" >&2; exit 1 ;;
esac

case "${1:-}" in
  --test) suffix="-test" ;;
  --release)
    suffix=""
    : "${VIRTUEDIT_CODESIGN_IDENTITY:?Set the Developer ID Application identity.}"
    : "${VIRTUEDIT_INSTALLER_IDENTITY:?Set the Developer ID Installer identity.}"
    : "${VIRTUEDIT_NOTARY_KEYCHAIN_PROFILE:?Set the notarytool keychain profile.}"
    ;;
  *)
    echo "Usage: $0 --test | --release" >&2
    exit 2
    ;;
esac

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/virtuedit-recorder-installer.XXXXXX")"
trap 'rm -rf -- "$work_dir"' EXIT

if [[ -n "${PLUGIN_ZIP:-}" ]]; then
  # Sign a plugin ZIP that CI built from the public source (the
  # VirtuEdit-OBS-Plugin-macOS-<arch> artifact) instead of building here.
  if [[ ! -f "$PLUGIN_ZIP" ]]; then
    echo "PLUGIN_ZIP not found: $PLUGIN_ZIP" >&2
    exit 1
  fi
  mkdir -p "$work_dir/stage"
  ditto -x -k "$PLUGIN_ZIP" "$work_dir/stage"
  expected_os="${MACOS_DEPLOYMENT_TARGET:-12.0}"
  for binary in Contents/MacOS/virtuedit-obs Contents/Resources/ffmpeg; do
    binary_path="$work_dir/stage/virtuedit-obs.plugin/$binary"
    archs="$(lipo -archs "$binary_path" 2>/dev/null || true)"
    minos="$(vtool -show-build "$binary_path" 2>/dev/null | awk '/minos/ { print $2; exit }')"
    if [[ "$archs" != "$MACOS_ARCHITECTURE" || "$minos" != "$expected_os" ]]; then
      echo "$binary in PLUGIN_ZIP is '$archs' (minimum macOS '$minos');" \
        "expected $MACOS_ARCHITECTURE (minimum macOS $expected_os)." >&2
      exit 1
    fi
  done
else
  # Build here, reusing the ZIP pipeline's bundle layout, license files, and
  # binary checks.
  PACKAGE_ROOT="$work_dir/bundle" \
    MACOS_ARCHITECTURE="$MACOS_ARCHITECTURE" \
    STAGE_DIR="$work_dir/stage" \
    ZIP_PATH="$work_dir/plugin.zip" \
    bash "$ROOT_DIR/scripts/package-macos.sh"
fi
bundle="$work_dir/stage/virtuedit-obs.plugin"
version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$bundle/Contents/Info.plist")"
minimum_os="${MACOS_DEPLOYMENT_TARGET:-12.0}"
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ||
      ! "$minimum_os" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
  echo "Invalid package version or minimum macOS version." >&2
  exit 1
fi

if [[ -z "$suffix" ]]; then
  # Sign the bundled executable before sealing the plugin bundle.
  codesign --force --timestamp --options runtime --sign "$VIRTUEDIT_CODESIGN_IDENTITY" \
    "$bundle/Contents/Resources/ffmpeg"
  codesign --force --timestamp --options runtime --sign "$VIRTUEDIT_CODESIGN_IDENTITY" "$bundle"
  codesign --verify --deep --strict "$bundle"
fi

cat > "$work_dir/components.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
  <dict>
    <key>RootRelativeBundlePath</key><string>virtuedit-obs.plugin</string>
    <key>BundleIsRelocatable</key><false/>
    <key>BundleIsVersionChecked</key><true/>
    <key>BundleHasStrictIdentifier</key><true/>
    <key>BundleOverwriteAction</key><string>upgrade</string>
  </dict>
</array>
</plist>
PLIST

package_id="com.virtuedit.obs-plugin.installer"
pkgbuild --root "$work_dir/stage" --component-plist "$work_dir/components.plist" \
  --identifier "$package_id" --version "$version" \
  --install-location "/Library/Application Support/obs-studio/plugins" \
  "$work_dir/plugin.pkg"

mkdir "$work_dir/resources"
# Shown on the first and last Installer screens. The Distribution below
# installs into the current user's home, so the plugin lands in
# ~/Library/Application Support/obs-studio/plugins/.
cat > "$work_dir/resources/Welcome.txt" <<WELCOME
Virtual Recorder for React Central $version

An OBS Studio plugin that records your reaction, the YouTube video
you react to, and a sync file in one session folder.

Before you continue:
- Quit OBS Studio. If it is open, the installer will ask you to close it.
- Requires OBS Studio 32.1.2 and macOS $minimum_os or later.

The plugin is installed for your user account only.

Virtual Recorder is an independent third-party plugin for OBS Studio.
It is not developed or endorsed by the OBS Project.
WELCOME

cat > "$work_dir/resources/Conclusion.txt" <<'CONCLUSION'
Virtual Recorder is installed.

Next
1. Open OBS Studio.
2. Virtual Recorder appears as a dock.
   If it is hidden, choose Tools > Open Virtual Recorder.

Update
Quit OBS Studio and open the new installer. It replaces this version.

Uninstall
Quit OBS Studio, then move this folder to the Trash:
~/Library/Application Support/obs-studio/plugins/virtuedit-obs.plugin
(In Finder, choose Go > Go to Folder and paste the path.)
Your recordings are not removed.

Help: help@virtuedit.com
License (GPL-2.0-or-later) and source code:
https://github.com/tnrud99/virtual-recorder-for-obs
CONCLUSION

cat > "$work_dir/Distribution.xml" <<DIST
<?xml version="1.0" encoding="UTF-8"?>
<installer-gui-script minSpecVersion="2">
  <title>Virtual Recorder for React Central</title>
  <welcome file="Welcome.txt" mime-type="text/plain"/>
  <conclusion file="Conclusion.txt" mime-type="text/plain"/>
  <options customize="never" hostArchitectures="$MACOS_ARCHITECTURE" require-scripts="false"/>
  <domains enable_anywhere="false" enable_currentUserHome="true" enable_localSystem="false"/>
  <volume-check><allowed-os-versions><os-version min="$minimum_os"/></allowed-os-versions></volume-check>
  <choices-outline><line choice="recorder"/></choices-outline>
  <choice id="recorder" title="Virtual Recorder for React Central">
    <pkg-ref id="$package_id"/>
  </choice>
  <pkg-ref id="$package_id">plugin.pkg</pkg-ref>
  <pkg-ref id="$package_id"><must-close><app id="com.obsproject.obs-studio"/></must-close></pkg-ref>
</installer-gui-script>
DIST

sign_args=()
if [[ -z "$suffix" ]]; then
  sign_args=(--sign "$VIRTUEDIT_INSTALLER_IDENTITY" --timestamp)
fi

filename="Virtual-Recorder-for-React-Central-${version}-macOS-${MACOS_ARCHITECTURE}${suffix}.pkg"
pkg="$work_dir/$filename"
productbuild --distribution "$work_dir/Distribution.xml" \
  --package-path "$work_dir" --resources "$work_dir/resources" \
  ${sign_args[@]+"${sign_args[@]}"} "$pkg"

if [[ -z "$suffix" ]]; then
  pkgutil --check-signature "$pkg"
  xcrun notarytool submit "$pkg" --keychain-profile "$VIRTUEDIT_NOTARY_KEYCHAIN_PROFILE" --wait
  xcrun stapler staple "$pkg"
  xcrun stapler validate "$pkg"
  spctl --assess --type install --verbose=2 "$pkg"
fi

mkdir -p "$OUTPUT_DIR"
mv -f "$pkg" "$OUTPUT_DIR/$filename"
echo "Built: $OUTPUT_DIR/$filename"
