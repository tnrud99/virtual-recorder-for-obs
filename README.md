# Virtual Recorder for React Central

An OBS Studio plugin that records a reaction session in one go: your webcam
and microphone, the YouTube video you are reacting to, and a sync file that
remembers every play, pause, and seek.

Virtual Recorder is an independent third-party plugin for OBS Studio. It is
not developed or endorsed by the OBS Project.

## What you get after recording

Each recording creates one session folder, `Virtual Recorder/<yyMMdd_title>/`,
containing:

| File | What it is |
|---|---|
| `..._reaction.mp4` (or `.mov` / `.mkv`) | Your reaction: camera and the audio sources you selected |
| `..._sync.json` | The playback timeline and layout, used by VirtuEdit tools |
| `..._combined.<ext>` | What you saw in OBS, saved with your normal OBS recording settings |

Keep the `_reaction` video and the `_sync.json` file together.

## Requirements

- OBS Studio **32.1.2** (this is the version we tested; other versions are not verified)
- macOS 12 or later (Apple Silicon or Intel) or Windows 10/11 (64-bit)
- Internet access while recording, to load the YouTube player

## Install

### macOS

1. Quit OBS Studio.
2. Open the `.pkg` that matches your Mac (Apple Silicon or Intel) and follow the prompts.
3. Open OBS Studio. Virtual Recorder appears as a dock. If it is hidden, choose
   **Tools > Open Virtual Recorder**.

### Windows

1. Quit OBS Studio.
2. Extract the downloaded ZIP (right-click > **Extract All**).
3. Double-click **Install-Windows.bat**.
4. If Windows asks "Do you want to allow this app to make changes?", click
   **Yes**. The publisher may show as unknown; administrator permission is
   needed to copy files into `C:\ProgramData`.
5. Wait for "Installation complete", then press any key to close the window.
6. Open OBS Studio. If the dock is hidden, choose **Tools > Open Virtual Recorder**.

**If you would rather not run the `.bat` file**, copy the `virtuedit-obs`
folder from the extracted ZIP into `C:\ProgramData\obs-studio\plugins\`
(create the `plugins` folder if it does not exist), then open OBS Studio.
The `.bat` file only copies files; you can open it in Notepad to check.

If Windows shows a blue "Windows protected your PC" screen, click
**More info** first; a **Run anyway** button then appears. This screen
appears because the file is new and not yet widely downloaded. You can
compare the file's SHA-256 hash with the one on the download page to confirm
the file is unchanged.

## Use

1. Set up the OBS scene you want to record: camera, overlays, and audio.
2. **Step 1 - OBS Scene & Audio:** pick the audio sources to save in the
   reaction video, then add YouTube Controls to your scene.
3. **Step 2 - YouTube Video:** paste a YouTube link.
4. **Step 3 - Position & Size:** place YouTube Controls where you want it.
5. **Step 4 - Reaction Video:** check the format and quality.
6. **Step 5 - Record:** start recording from the dock, then play, pause, and
   seek in YouTube Controls. Stop recording from the same dock.

### Audio tip

Keep YouTube's sound out of the reaction video. If it plays through speakers,
your microphone will pick it up, so headphones are recommended. If a selected
audio source looks like system or loopback audio, the dock shows a warning;
recording is not blocked, so check your OBS audio routing yourself.

## Update

When a newer compatible version is released, the dock shows
**Update X available - Download**. The plugin never downloads or installs
anything by itself. To update, quit OBS, download the new version, and
install it the same way as before; it replaces the old one.

## Uninstall

- **macOS:** quit OBS and move
  `~/Library/Application Support/obs-studio/plugins/virtuedit-obs.plugin`
  to the Trash (in Finder, **Go > Go to Folder** and paste the path).
- **Windows:** quit OBS and double-click **Uninstall-Windows.bat**, or delete
  `C:\ProgramData\obs-studio\plugins\virtuedit-obs`.

Your recordings are not removed.

## Privacy

- Recordings stay on your computer in the folder you chose. The plugin does not upload them.
- Recording loads a player page from `virtuedit.com` and the YouTube player,
  so those services receive normal web requests.
- Once each time OBS starts, the plugin fetches `https://virtuedit.com/recorder-update.json`
  to see whether a newer version exists.
- Make sure you have the rights to the video you react to and to the people who appear in your recording.

## Troubleshooting

- **The dock does not appear:** choose **Tools > Open Virtual Recorder**, or
  check **Docks** in the OBS menu.
- **"obs-player version mismatch" in the OBS log:** update the plugin to the latest version.
- **Recording will not start:** make sure the OBS recording path is a local
  folder. Output to a URL only cannot produce the combined video.
- **Anything else:** email help@virtuedit.com and attach the OBS log
  (**Help > Log Files > Upload Current Log File**, or open the log folder).

## License and source code

Virtual Recorder is free software under the **GNU GPL, version 2 or later**.
See [LICENSE](LICENSE). The source code for each release is at
https://github.com/tnrud99/virtual-recorder-for-obs. Bundled components
(FFmpeg, libwebsockets) and their sources are listed in
[SOURCE_CODE.md](SOURCE_CODE.md) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).

## For developers

Build, packaging, and release instructions are in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). The sync file format is in
[docs/OBS_SYNC_JSON_V2_CONTRACT.md](docs/OBS_SYNC_JSON_V2_CONTRACT.md).
