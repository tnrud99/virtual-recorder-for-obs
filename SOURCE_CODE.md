# Source Code Availability

Virtual Recorder is distributed under GPL-2.0-or-later. The source code for
each plugin release is available from the Git tag that matches the release:

https://github.com/tnrud99/virtual-recorder-for-obs

The release source archive includes the plugin source and the scripts used to
build and package it.

## Bundled and linked components

### FFmpeg for macOS and Windows

- Version: 7.0.2
- macOS source archive: https://ffmpeg.org/releases/ffmpeg-7.0.2.tar.xz
- Windows source archive: https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n7.0.2.tar.gz
- Build configuration: `scripts/build-ffmpeg-lgpl.sh` in the matching plugin
  source release

### libwebsockets

- Version: 4.3.10
- Source: https://github.com/warmcat/libwebsockets/tree/v4.3.10
- Build configuration: `CMakeLists.txt` in the matching plugin source release

Qt and OBS Studio are supplied by the user's OBS Studio installation and are
not redistributed in the plugin archives.
