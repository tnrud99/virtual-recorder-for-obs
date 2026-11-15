# Virtual Recorder OBS Release Checklist

Use this for the full OBS verification pass before shipping a release build.
For the shorter pre-test pass, use `OBS_RELEASE_SMOKE_TEST.md`.

## Automated Checks

- Run release preflight before manual OBS testing:
  - `scripts/release-preflight.sh`
- After a successful smoke recording, save a final diagnostics report:
  - `REQUIRE_RECORDING=1 scripts/release-diagnostics.sh`
- Build the plugin:
  - macOS: `scripts/build-macos.sh`
  - Windows CI/local: `scripts/build-windows.ps1`
- Run C++ tests:
  - macOS/local: `ctest --test-dir build --output-on-failure`
  - Windows: `ctest --test-dir build-windows -C Release --output-on-failure`
- Validate JSON fixtures:
  - `node tests/validate-obs-sync-json.mjs tests/fixtures/*.json`
- Package release artifacts only after the checks above pass.
- Package macOS release:
  - `BUILD_FFMPEG=1 scripts/package-macos.sh`
- Package Windows release:
  - `scripts/package-windows.ps1 -BuildDir build-windows -FfmpegExe C:\path\to\ffmpeg.exe`
- Download the latest Windows CI artifact when using GitHub Actions:
  - `scripts/download-latest-windows-artifact.sh`
- Verify built release ZIPs:
  - `scripts/verify-release-archives.sh`
- For Windows release ZIP, confirm the extracted folder contains:
  - `Install-Windows.bat`
  - `Uninstall-Windows.bat`
  - `How to Install.txt`
  - `virtuedit-obs/bin/64bit/virtuedit-obs.dll`
  - `virtuedit-obs/LICENSE.txt`
  - `virtuedit-obs/THIRD_PARTY_LICENSES.txt`
  - `virtuedit-obs/SOURCE_CODE.txt`
  - `virtuedit-obs/licenses/LGPL-2.1.txt`
  - `virtuedit-obs/licenses/LGPL-3.0.txt`
- For macOS release archive, confirm it contains an installable `.plugin`
  bundle, not the raw CMake `bin/` and `data/` install layout.
- Confirm the macOS plugin resources contain `LICENSE.txt`,
  `THIRD_PARTY_LICENSES.txt`, `SOURCE_CODE.txt`, and both LGPL license texts.
- Create the GitHub release from a tag matching the plugin version and confirm
  its source archives are available beside the binary ZIPs.
- If publishing in OBS Resources, include the required third-party and
  AI-assisted-development disclosure in the resource description.
- For release packaging, run `scripts/build-ffmpeg-lgpl.sh` first.
- Confirm bundled `ffmpeg -version` does not include `--enable-gpl`.
- Confirm Windows ZIP includes `virtuedit-obs/data/ffmpeg.exe`.
- Confirm macOS `.plugin` includes `Contents/Resources/ffmpeg`.
- Confirm removing the bundled ffmpeg causes a clear remux failure instead of silently using system ffmpeg.

## Hosted Player

- Confirm `web/obs-player.html` is mirrored to the website repo before release.
- Confirm deployed `https://virtuedit.com/obs-player.html` has the expected `PLAYER_VERSION`.
- Confirm OBS logs do not show `obs-player version mismatch`.
- Confirm OBS logs do not show `obs-player is missing session token`.
- Confirm OBS logs select only one timestamp producer per recording.

## Install And Dock

- Test fresh install.
- Test update over an existing plugin install.
- On macOS, verify the installed plugin before opening OBS:
  - `scripts/verify-macos-install.sh`
- Restart OBS Studio after install/update.
- Confirm `Virtual Recorder` appears as its own OBS dock.
- Confirm it docks on the right after OBS restart even if it was closed or
  undocked previously.
- Confirm `Tools > Open Virtual Recorder` raises the dock without forcing a
  new dock side mid-session.
- Confirm the dock remains usable around 320-380 px width and scrolls instead of clipping controls.
- Confirm there is no separate `?` guide; guidance is shown in the relevant setup steps.

## Step Flow

- Confirm the visible workflow is:
  - `Step 1 - OBS Scene & Audio`
  - `Step 2 - YouTube Video`
  - `Step 3 - Position & Size`
  - `Step 4 - Reaction Video`
  - `Step 5 - Record`
- Step 1:
  - Confirm the intro explains that selected audio is saved in the Reaction Video and Combined Video follows normal OBS recording settings.
  - Confirm the field labels read `Current scene` and `Audio to save`.
  - Confirm `Audio to save` uses the full panel width and does not show a horizontal scrollbar in a narrow dock.
  - Confirm global OBS audio sources appear as `Global - <source name>` while other audio sources keep their OBS source names.
  - Confirm audio sources that exist only in other OBS scenes do not appear in `Audio to save`.
  - Confirm Virtual Recorder YouTube Controls sources do not appear in `Audio to save`, including numbered duplicates from other OBS scenes.
  - Confirm no audio source is auto-selected on first setup; the user must choose what should be saved.
  - With an existing saved audio selection, confirm the saved selection is preserved.
  - Confirm the setup copy explains that this selection becomes the Reaction Video audio and should contain the user's voice or reaction.
  - Confirm `Refresh list` is a subtle helper near the scene/audio list, not a competing primary action.
  - Select one or more audio source(s) to save in the Reaction Video.
  - Confirm `Add YouTube Controls` creates exactly one visible YouTube Controls box in the selected scene.
  - After YouTube Controls already exists in the current scene, confirm the primary button changes to `Save Setup`.
- Step 2:
  - Paste a valid YouTube URL and click `Load`.
  - While loading, confirm the URL field is read-only, `Load` changes to `Loading...`, the status says the video is loading in YouTube Controls, and duplicate load clicks are blocked.
  - Confirm the YouTube Controls window opens automatically while loading.
  - Confirm successful load stays on `Step 2 - YouTube Video`.
  - Confirm the loaded URL remains visible in the link field.
  - Confirm the primary next action is `Next` and `Change Video` opens the URL field.
  - Confirm `Skip to Recording` appears as a smaller outline secondary button under `Next` only after the loaded video and current setup are valid, and jumps to `Step 5 - Record`.
- Step 3:
  - Click `Next` from Step 2 and confirm `Step 3 - Position & Size` appears.
  - Confirm the page focuses on positioning and resizing YouTube Controls in OBS Scene Preview.
  - Restart OBS with a loaded YouTube Controls source and confirm the dock opens at Step 2 with the loaded video link instead of showing an empty URL field.
  - Confirm the next action reads `Next`.
- Step 4:
  - Confirm the page shows the current OBS recording setting and the resulting Reaction Video setting.
  - Confirm the page allows direct Reaction Video adjustment: video bitrate for CBR, or CRF for quality-based output.
  - Confirm `Audio bitrate` can be adjusted directly.
  - Confirm changing a value updates the resulting Reaction Video setting and persists after restarting OBS.
  - Confirm `Reset to OBS Values` returns Reaction Video format, quality, and audio bitrate to the current OBS values.
  - Confirm the next action reads `Next`.
- Step 5:
  - Confirm the record setup panel is titled `Before Recording`.
  - Confirm `Open YouTube Controls`, `YouTube Volume`, audio warning, and `Start Recording` are visible in that order.
  - Confirm `Recording details` explains that Combined Video uses the current OBS recording settings.
  - Open YouTube Controls and confirm the button changes to `Reopen YouTube Controls`.
  - Confirm `Start Recording` is disabled until the video is loaded, YouTube Controls is valid, YouTube Controls has been opened, disk space is sufficient, and OBS is not already recording.
  - Start a manual OBS recording and confirm the dock says to stop OBS recording, then start from Virtual Recorder.
- Navigation:
  - From Step 5, `Back` returns to Step 4.
  - From Step 4, `Back` returns to Step 3.
  - From Step 3, `Back` returns to Step 2.
  - From Step 2, `Back` opens Step 1 setup.
  - Confirm Step 1 setup does not show its own `Back` button.
- Completion actions:
  - `Record Same Video Again` returns to Step 2 with the same YouTube video loaded.
  - Confirm `Record Same Video Again` is the red primary action and sits above the secondary action.
  - Confirm `Record Same Video Again` is disabled if no loaded YouTube video is available.
  - `Record Different Video` clears the URL/video and returns to Step 2.

## YouTube URL And Load Failures

- Paste watch, youtu.be, Shorts, Live, embed, and mobile YouTube links and confirm valid videos load.
- Paste an invalid YouTube link and confirm the dock asks for a supported YouTube link.
- After loading a valid video, edit the URL and confirm the current video can be kept.
- Try loading a second video that fails and confirm the previous loaded video is restored.
- Load a video with embedding disabled and confirm the dock explains another video is needed for sync timing.
- Confirm `Open YouTube Controls` before a video load explains that the video must be loaded first.

## OBS Scene And Source Defense

- Delete YouTube Controls from the OBS scene and confirm Start is blocked.
- Click `Add YouTube Controls` / `Fix YouTube Controls` and confirm one visible YouTube Controls box is restored.
- Confirm the repair hint explains it only fixes Virtual Recorder's YouTube Controls in the current OBS scene and leaves other OBS sources unchanged.
- Confirm `Recording details` distinguishes these states: not in scene, hidden, duplicate boxes, and ready.
- Switch to an OBS scene without YouTube Controls and confirm the dock returns to `Step 1 - OBS Scene & Audio` with `Add YouTube Controls`, not Step 2 or any YouTube URL controls.
- Confirm duplicate YouTube Controls shows `Fix YouTube Controls`, not `Show YouTube Controls`.
- Hide YouTube Controls and confirm Start is blocked with a visible-source warning.
- Duplicate YouTube Controls and confirm Start is blocked with a one-source warning.
- Rename YouTube Controls and confirm the plugin still recognizes or repairs it.
- Open YouTube Controls properties and change the URL; confirm Start repairs it and requires reopening YouTube Controls before recording.
- Change YouTube Controls to local file mode; confirm Start repairs it and requires reopening YouTube Controls before recording.
- Change the browser source width/height; confirm Start repairs the source settings.
- Switch OBS scenes before recording and confirm the dock updates to the current scene.
  - If YouTube Controls is missing, hidden, or duplicated in the new scene, confirm the dock gives the matching action.
  - If YouTube Controls is valid, confirm the dock asks the user to check selected audio and the YouTube Controls position.
- Switch OBS scenes during recording and confirm the plugin immediately switches back to the recording scene and warns the user.
- Confirm scene items unlock after a start failure.
- Lock the camera or Player Controls before recording and confirm its original
  locked state is preserved after stop and after a start failure.
- Leave the same items unlocked before recording and confirm they return to the
  unlocked state after stop.
- Confirm audio mixer assignments are restored after a start failure.

## Recording Flow

- Start recording from Step 5.
- Confirm reaction recording and sync start only after OBS combined recording is active.
- Force a pre-recording setup failure if possible and confirm no stale saved file or empty session folder is shown as a completed recording.
- Confirm the YouTube Controls window is used for play, pause, and seek.
- During recording, test:
  - play
  - pause
  - paused seek
  - playing seek
  - end
  - stop
- Confirm the recording page updates timer and sync timing count.
- Click `Stop Recording` once and confirm the button changes to `Finalizing...` and the recording page keeps saying files are finalizing until the complete screen opens.
- Confirm `Start Recording` cannot start another recording while files are finalizing.
- Confirm repeated stop clicks do not create duplicate finalize work.
- Confirm stale timers/callbacks from one recording do not update a later recording.
- Quit OBS while recording is active and confirm shutdown does not crash and the
  reaction MKV remains available for recovery.
- Quit OBS while reaction remux is finalizing and confirm shutdown does not crash.
- Reload/unload the plugin and confirm the Virtual Recorder dock is removed cleanly.
- After a successful smoke recording, run:
  - `REQUIRE_RECORDING=1 scripts/check-obs-log.sh`

## Audio

- The Reaction Video contains the intended selected OBS audio source(s).
- The Reaction Video should not contain YouTube audio.
- Confirm speaker bleed guidance appears before and during recording.
- Confirm risky audio selections show a warning but do not block recording.
- Confirm Desktop Audio is not described as universally forbidden.
- Confirm Combined Video contains the intended OBS mixed scene audio.
- Confirm Combined Video preserves every audio track selected in the current
  OBS recording settings.
- Configure OBS recording to use all six audio tracks and confirm Virtual
  Recorder blocks recording with a clear message instead of changing any track.
- Confirm OBS mixer assignments are restored after stop.

## Outputs And Completion Screen

- Confirm a session folder is created under the OBS recording path:
  - `Virtual Recorder/<yyMMdd_title-slug>/`
- After a successful smoke recording, run:
  - `scripts/check-latest-recording.sh`
- If OBS saved to a different location, set `VIRTUAL_RECORDER_DIR` or `SESSION_DIR` for that command.
- Confirm output files share the same base filename:
  - `<base>_reaction.mp4`, `<base>_reaction.mov`, or `<base>_reaction.mkv`
  - `<base>_sync.json`
  - `<base>_combined.<ext>`
- Confirm zero-byte files are not shown as `Saved`.
- Confirm Reaction Video shows:
  - `Saved` for MP4
  - `Saved as MOV` for MOV
  - `Saved as MKV` for selected MKV or when conversion fails but MKV exists
  - `Failed` when no usable reaction file exists
- Confirm Sync File shows:
  - `Saved (N)` when sync timing exists
  - `Saved, no sync` when no play/pause/seek timing exists, while JSON is still saved
  - Confirm the `Saved, no sync` tooltip explains that the Sync File was saved but no play/pause/seek timing was recorded.
  - `Failed` / `Not saved` if JSON cannot be written
  - Confirm Sync File does not stay on `Finalizing...` after YouTube Controls confirms recording stopped.
- Confirm Combined Video shows:
  - `Saved` for any usable file produced by the current OBS recording settings
  - `Not saved` when OBS does not report a usable combined file
- Confirm the Combined Video keeps the OBS-selected container extension and is
  not converted to MP4 by Virtual Recorder.
- Confirm the Combined Video keeps the OBS-selected encoder, resolution, frame
  rate, quality/bitrate, and audio-track layout.
- In Advanced Output > Custom Output (FFmpeg), enable file output and confirm
  the session folder is created under the configured FFmpeg file path.
- Switch Custom Output (FFmpeg) to URL-only output and confirm Start is blocked
  with a clear message because no local Combined Video can be created.
- Confirm `Failed` and `Not saved` statuses use an attention/error style, not the softer MKV warning style.
- Confirm `Failed` / `Not saved` tooltips explain what was not saved and what the user should check next, including recording folder, OBS recording settings, disk space, file permissions, or security software where relevant.
- Confirm long failure tooltips are line-broken into short troubleshooting items, not one dense sentence.
- Confirm `Finalizing...` tooltips explain which file is still being written or confirmed.
- Confirm `Recording Complete - Check Files` appears when any file status needs attention.
- Confirm normal full success shows `Recording Complete`.
- Confirm `Saved to:` is prominent and points to the session folder.
- Confirm `Open Folder` is disabled until a recording folder exists and its tooltip shows the recording folder path when enabled.
- Confirm `Open Folder` opens the session folder.
- On Windows 10 and 11, confirm dock text uses the Windows UI font stack and
  shows English, Korean device names, punctuation, and status text without
  missing-glyph boxes at 100%, 125%, and 150% display scaling.
- Narrow the OBS dock and confirm saved file rows stack before status text gets cramped or clipped.
- In the narrow dock, confirm the `Next recording` actions are stacked, visually separate from the saved file statuses, and the same-video action stays primary.
- Confirm the complete screen does not show an editor or web editor button.

## JSON Contract

- Validate each produced sync file:
  - `node tests/validate-obs-sync-json.mjs "/path/to/recording_sync.json"`
- Confirm:
  - `source` is `obs-plugin`
  - `schema_version` is `2`
  - `reaction_video` matches the finalized reaction filename
  - `composite_video` matches the OBS combined filename when available
  - `layout.overlay_target` is `original`
  - `layout.overlay_position` is `custom`
  - `layout.overlay_x/y/width/height` match the OBS YouTube box
  - `layout.overlay_preset_fallback` exists and is the nearest supported fallback preset
  - `sync_points` include `start` and `end`
  - play/pause pairs are normalized to match reaction/youtube deltas within the expected tolerance
  - `youtube_volume` matches the final YouTube Volume value

## Windows Smoke Test

- Install from the Windows ZIP using `Install-Windows.bat` as administrator.
- Before opening OBS, run:
  - `powershell -ExecutionPolicy Bypass -File Verify-Windows-Install.ps1`
- Confirm hidden folders are not required when using the installer.
- Confirm OBS detects the plugin after restart.
- Confirm the dock opens and is not clipped at common Windows dock widths.
- Complete one short recording.
- Confirm Reaction Video, Sync File, and Combined Video statuses are correct.
- Confirm MKV, MOV, or MP4 combined output is accepted as `Saved` without
  container conversion.
- Confirm `Open Folder` opens the Windows session folder.

## Downstream Smoke Test

- Open the current editor/viewer app manually.
- Load the produced `_sync.json`.
- Connect the produced reaction video.
- Confirm the original YouTube video is inferred from `youtube_video_id`.
- Confirm sync playback is usable.
- Confirm custom overlay fields are preserved on import/export.
