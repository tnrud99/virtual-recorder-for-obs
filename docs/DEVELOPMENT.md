# Development and Release Guide

Maintainer notes for building, packaging, and releasing Virtual Recorder. End-user instructions are in the [README](../README.md).

## Verified targets

Each plugin build is compiled and tested against an explicit OBS version (currently OBS Studio 32.1.2; see `buildspec.json`). CI checks the OBS latest-stable release weekly and fails clearly when the verified pin needs a compatibility update. Future OBS releases are verified before the supported version in this repository is updated.

Each release archive includes the GPL/LGPL license texts, third-party notices, and exact source/build references for the bundled FFmpeg and statically linked libwebsockets components. See `SOURCE_CODE.txt` inside the plugin package.

## Update notices (release maintainers)

Recorder checks `https://virtuedit.com/recorder-update.json` once after OBS
starts. A compatible newer release adds a Download link to the dock before
recording. Open the link, quit OBS, and install the new package.

For release maintainers, `web/recorder-update.json` is the empty starting
manifest for that HTTPS endpoint. After publishing and checking the download,
add or replace the entry for its platform under `releases`. An omitted
platform has no update notice. Example for the `macos-arm64` entry:

```json
{
  "version": "1.0.1",
  "minObsVersion": "32.1.2",
  "maxObsVersion": "32.1.2",
  "minOsVersion": "12.0",
  "downloadUrl": "https://virtuedit.com/download#recorder"
}
```

Use `windows-x64` for Windows or `macos-x64` for Intel Macs, and set each platform's OBS version range
(inclusive) and minimum OS to its verified requirements. The link may point
to the VirtuEDIT download page or to a release in `tnrud99/virtuedit-downloads`.
Use the public HTTPS endpoint on the same origin, with schema `1`.

## macOS Build

Prerequisites:

- OBS Studio installed in `/Applications/OBS.app`
- Qt 6.8.3 and Homebrew simde. For release builds, use the pinned
  `macos-deps-qt6-2025-08-23-arm64` package from `obsproject/obs-deps`, which
  matches OBS 32.1.2 and supports macOS 12. Set `QT_PREFIX` or `QT_ROOT_DIR`
  to the extracted package root:

```bash
brew install simde
export QT_PREFIX="/path/to/obs-qt"
```

- OBS Studio source headers matching the installed OBS version:

```bash
git clone --depth 1 --branch 32.1.2 https://github.com/obsproject/obs-studio.git /tmp/obs-studio-32.1.2
```

Build:

```bash
scripts/build-macos.sh
```

Package the OBS-ready macOS ZIP:

```bash
BUILD_FFMPEG=1 scripts/package-macos.sh
```

The package is written to:

```text
release/macos/VirtuEdit-OBS-Plugin-macOS-arm64.zip
```

## Windows Build

The GitHub Actions Windows job builds the plugin with:

- OBS Studio 32.1.2 headers
- OBS Studio 32.1.2 Windows runtime DLLs
- Qt 6.8.3 x64 from the pinned OBS `windows-deps-qt6-2025-08-23-x64`
  dependency package
- SIMDe v0.8.2 headers

The runtime OBS ZIP does not include MSVC import libraries, so the CI job
generates `obs.lib` and `obs-frontend-api.lib` from the OBS runtime DLL exports
before building.

For a local Windows build, set:

```powershell
$env:OBS_SOURCE_DIR = "C:\path\to\obs-studio-32.1.2"
$env:OBS_LIB_DIR = "C:\path\to\obs-import-libs"
$env:SIMDE_INCLUDE_DIR = "C:\path\to\simde"
$env:CMAKE_PREFIX_PATH = "C:\path\to\obs-qt"
scripts\build-windows.ps1
```

Package the OBS-ready ZIP:

```powershell
scripts\package-windows.ps1 -BuildDir build-windows -FfmpegExe C:\path\to\ffmpeg.exe
```

From macOS, download and verify the latest successful Windows CI artifact:

```bash
scripts/download-latest-windows-artifact.sh
```

## JSON Contract Check

The v2 data contract for editor/server/viewer consumers is documented in
[OBS_SYNC_JSON_V2_CONTRACT.md](OBS_SYNC_JSON_V2_CONTRACT.md).

Before manual OBS release testing, run the release preflight:

```bash
scripts/release-preflight.sh
```

After a smoke recording, save a release diagnostics report:

```bash
REQUIRE_RECORDING=1 scripts/release-diagnostics.sh
```

After building release ZIPs, verify their contents:

```bash
scripts/verify-release-archives.sh
```

Run the C++ parser tests after building:

```bash
ctest --test-dir build --output-on-failure
```

Validate the OBS sync JSON fixtures, or pass a real `_sync.json` after
recording:

```bash
node tests/validate-obs-sync-json.mjs tests/fixtures/*.json
node tests/validate-obs-sync-json.mjs "/path/to/recording_sync.json"
```

## Hosted OBS Player

`https://virtuedit.com/obs-player.html` is deployed from the website repo:

```text
viewer/src/server/public/obs-player.html
```

Keep this repo's `web/obs-player.html` as the development mirror for plugin
changes, but update and deploy the website repo before testing or releasing a
plugin build that changes the player protocol. OBS logs should not show
`obs-player version mismatch` after deployment.

## macOS packaging and development install

To build a local installer, run `scripts/package-macos-installer.sh --test`.
For public distribution, use `--release` with
`VIRTUEDIT_CODESIGN_IDENTITY` (Developer ID Application),
`VIRTUEDIT_INSTALLER_IDENTITY` (Developer ID Installer), and
`VIRTUEDIT_NOTARY_KEYCHAIN_PROFILE`. The release command signs the payload
and installer, notarizes the package, and verifies it before writing it to
`release/macos/`.

Release packages are made from the bundles CI builds, so the shipped binaries
come from the public source and pinned dependencies and carry no local paths.
CI builds both `arm64` and `x86_64` (Intel is cross-compiled on Apple Silicon).
Download the `VirtuEdit-OBS-Plugin-macOS-arm64` and `-x86_64` artifacts from
the run for the release commit, then sign, package and notarize each one:

```bash
PLUGIN_ZIP=/path/to/VirtuEdit-OBS-Plugin-macOS-arm64.zip MACOS_ARCHITECTURE=arm64 \
  VIRTUEDIT_CODESIGN_IDENTITY="Developer ID Application: virtuedit Inc. (W9K9R7S6N4)" \
  VIRTUEDIT_INSTALLER_IDENTITY="Developer ID Installer: virtuedit Inc. (W9K9R7S6N4)" \
  VIRTUEDIT_NOTARY_KEYCHAIN_PROFILE=VirtuEDIT-Notary \
  scripts/package-macos-installer.sh --release
```

Repeat with the `x86_64` ZIP and `MACOS_ARCHITECTURE=x86_64`. The script
checks each binary's architecture and minimum macOS before signing.

To build an Intel package locally instead, set `MACOS_ARCHITECTURE=x86_64`,
point `OBS_LIB_DIR` at the Intel OBS Studio app's `Contents/Frameworks`, and
set `QT_PREFIX` to the OBS-matched Qt package (universal or x86_64). Use
`BUILD_FFMPEG=1` to build the matching Intel FFmpeg. Intel builds and FFmpeg
outputs use separate folders under `build/`.

For development builds installed directly into OBS:

```bash
scripts/install-macos-dev.sh
scripts/verify-macos-install.sh
```

This installs to:

```text
~/Library/Application Support/obs-studio/plugins/virtuedit-obs.plugin
```

Restart OBS Studio. `Virtual Recorder` opens as its own dock. If it is hidden,
open it from `Tools > Open Virtual Recorder`.

## Release verification

Use [OBS_RELEASE_SMOKE_TEST.md](../OBS_RELEASE_SMOKE_TEST.md) before sending a
test build to anyone. Use [OBS_E2E_CHECKLIST.md](../OBS_E2E_CHECKLIST.md) when
doing the full OBS workflow verification pass.

Viewer/editor follow-up work for custom OBS overlay coordinates is tracked in
[VIEWER_EDITOR_CUSTOM_OVERLAY_TODO.md](VIEWER_EDITOR_CUSTOM_OVERLAY_TODO.md).
