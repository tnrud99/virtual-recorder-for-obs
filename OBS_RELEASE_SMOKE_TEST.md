# Virtual Recorder Release Smoke Test

Use this short checklist before sending a test build to anyone. The full
release checklist is in `OBS_E2E_CHECKLIST.md`; this file is the minimum
clean-install pass.

## 1. Preflight

- Run:
  - `scripts/release-preflight.sh`
- Confirm it ends with:
  - `Release preflight passed.`
- If release ZIPs have already been built, run:
  - `scripts/verify-release-archives.sh`
- Confirm it ends with:
  - `Release archive verification passed.`

## 2. Install

macOS:

- Quit OBS Studio completely.
- Open the final `.pkg` and finish installation for the current user.
- Run `scripts/verify-macos-install.sh`.
- Reinstall over an earlier Recorder bundle and confirm that the installed
  version matches the package and OBS loads one Recorder dock.
- Confirm install verification passes.

Windows:

- Quit OBS Studio completely.
- Extract the Windows ZIP.
- Right-click `Install-Windows.bat`.
- Choose `Run as administrator`.
- Run:
  - `powershell -ExecutionPolicy Bypass -File scripts/verify-windows-install.ps1`
- Restart OBS Studio.

## 3. OBS Startup

- Open OBS Studio.
- Confirm `Virtual Recorder` appears as its own dock.
- Confirm it docks on the right side of OBS after restart even if it was
  closed or undocked in the previous session.
- If hidden during the session, open it from `Tools > Open Virtual Recorder`
  (shows it again without moving it; restart restores the right dock).
- Confirm the dock is not clipped at the default size.
- Confirm the first screen says the browser is only for copying the YouTube
  link, and YouTube Controls is used for playback and sync timing.

## 4. One Short Recording

- Use an OBS scene with a visible camera or test visual.
- Add or fix YouTube Controls from Step 1.
- Choose the audio source(s) to save in the Reaction Video.
- Confirm audio sources are not pre-labeled; choose the intended voice or reaction audio manually.
- Confirm global OBS audio sources are labeled `Global`.
- Load an embeddable YouTube video in Step 2.
- Confirm the YouTube Controls window opens while loading.
- Confirm Step 2 stays visible with the loaded link and `Next`.
- Confirm `Skip to Recording` appears as a smaller outline button under `Next`
  (secondary to `Next`, not a text link), then continue with `Next` for the full flow.
- Click `Next`.
- Confirm `Step 3 - Position & Size` appears.
- Position and resize YouTube Controls in OBS Scene Preview if needed, then click `Next`.
- Confirm `Step 4 - Reaction Video` shows the current OBS recording setting.
- Adjust the Reaction Video bitrate or quality if needed, then click `Next`.
- Click `Open YouTube Controls` from Step 5.
- Confirm the Step 5 button changes to `Reopen YouTube Controls`.
- Press `Start Recording`.
- In YouTube Controls, test:
  - play
  - pause
  - seek
- Record for at least 10 seconds.
- Press `Stop Recording`.

## 5. Completion Screen

- Confirm the title is `Recording Complete` for a full success, or
  `Recording Complete - Check Files` if any file needs attention.
- Confirm `Saved to:` points to the session folder.
- Confirm file statuses are accurate:
  - Reaction Video: `Saved`, `Saved as MOV`, or `Saved as MKV`
  - Sync File: `Saved (N)`
  - Combined Video: `Saved` in the container selected in OBS
- Click `Open Folder`.

## 6. File Check

- Run:
  - `scripts/check-latest-recording.sh`
- If OBS saved to a different location, run:
  - `VIRTUAL_RECORDER_DIR="/path/to/Virtual Recorder" scripts/check-latest-recording.sh`
- If you already have the session folder open, run:
  - `SESSION_DIR="/path/to/session-folder" scripts/check-latest-recording.sh`
- Confirm the session folder contains:
  - `<base>_reaction.mp4`, `<base>_reaction.mov`, or `<base>_reaction.mkv`
  - `<base>_sync.json`
  - `<base>_combined.<ext>`
- Confirm no file is zero bytes.
- Open the Reaction Video and confirm it does not contain YouTube audio.
- Open the Combined Video and confirm it contains the intended full OBS scene.
- Confirm the Combined Video follows the current OBS recording settings.
- Confirm the Combined Video extension matches the OBS-selected container and
  was not converted by Virtual Recorder.
- If Advanced Custom Output (FFmpeg) is supported in the release profile,
  confirm file output uses its configured file path and URL-only output is
  blocked before recording starts.

## 7. Repeat Flow

- On Windows, quickly inspect the dock at the machine's current display scale:
  text should be crisp, fit its controls, and show no missing-glyph boxes.

- Click `Record Same Video Again`.
- Confirm it returns to `Step 2 - YouTube Video` with the same video loaded.
- Click `Record Different Video`.
- Confirm it returns to Step 2 with the URL cleared.

## 8. OBS Log Check

- Close OBS Studio after the smoke recording.
- Run:
  - `REQUIRE_RECORDING=1 scripts/check-obs-log.sh`
- Confirm it ends with:
  - `OBS log verification passed.`

## 9. Release Diagnostics

- Run:
  - `REQUIRE_RECORDING=1 scripts/release-diagnostics.sh`
- Confirm it ends with:
  - `Release diagnostics passed.`
- Keep the generated report path with the test build notes.

## Pass Criteria

- Preflight passes.
- Installed plugin verification passes where available.
- OBS starts with the dock visible or available from Tools.
- One short recording produces usable Reaction Video, Sync File, and Combined
  Video.
- Sync JSON validates.
- No YouTube audio is heard in the Reaction Video.
- Re-record and different-video flows return to the expected steps.
- OBS log verification passes after the smoke recording.
- Release diagnostics passes and saves a report.
