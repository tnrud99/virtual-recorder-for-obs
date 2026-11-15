#!/usr/bin/env bash
set -euo pipefail

# ──────────────────────────────────────────────────────────────
# Build a minimal LGPL-2.1 ffmpeg for remux-only use (-c copy).
# No encoders/decoders/filters — just container muxing/demuxing.
# Output: a single ffmpeg binary (~2-5 MB).
#
# Usage:
#   ./scripts/build-ffmpeg-lgpl.sh          # build for current arch
#   FFMPEG_VERSION=7.1 ./scripts/...        # override version
# ──────────────────────────────────────────────────────────────

FFMPEG_VERSION="${FFMPEG_VERSION:-7.0.2}"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-12.0}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MACOS_ARCHITECTURE="${MACOS_ARCHITECTURE:-$(uname -m)}"
host_os="$(uname -s)"
arch_suffix=""
ffmpeg_cc=clang
ffmpeg_name=ffmpeg
if [[ "$host_os" == MINGW64* || "$host_os" == UCRT64* ]]; then
  arch_suffix="-windows-x64"
  ffmpeg_cc=gcc
  ffmpeg_name=ffmpeg.exe
fi
if [[ "$host_os" == "Darwin" ]]; then
  case "$MACOS_ARCHITECTURE" in
    arm64) ffmpeg_arch=aarch64 ;;
    x86_64) ffmpeg_arch=x86_64; arch_suffix="-x86_64" ;;
    *) echo "Unsupported Mac architecture: $MACOS_ARCHITECTURE" >&2; exit 1 ;;
  esac
fi
BUILD_DIR="$ROOT_DIR/build/ffmpeg-lgpl$arch_suffix"
INSTALL_DIR="$ROOT_DIR/build/ffmpeg-dist$arch_suffix"
ARCHIVE="ffmpeg-${FFMPEG_VERSION}.tar.xz"
SRC_DIR="$BUILD_DIR/ffmpeg-${FFMPEG_VERSION}"
SOURCE_URL="https://ffmpeg.org/releases/$ARCHIVE"
SOURCE_SHA256="8646515b638a3ad303e23af6a3587734447cb8fc0a0c064ecdb8e95c4fd8b389"
if [[ "$ffmpeg_name" == "ffmpeg.exe" ]]; then
  ARCHIVE="ffmpeg-n${FFMPEG_VERSION}.tar.gz"
  SRC_DIR="$BUILD_DIR/FFmpeg-n${FFMPEG_VERSION}"
  SOURCE_URL="https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n${FFMPEG_VERSION}"
  SOURCE_SHA256="5eb46d18d664a0ccadf7b0adee03bd3b7fa72893d667f36c69e202a807e6d533"
fi

# ── Download source ──────────────────────────────────────────
mkdir -p "$BUILD_DIR"
if [[ ! -f "$BUILD_DIR/$ARCHIVE" ]]; then
  echo "==> Downloading ffmpeg ${FFMPEG_VERSION}..."
  curl -fSL --retry 3 --retry-all-errors "$SOURCE_URL" -o "$BUILD_DIR/$ARCHIVE.partial"
  mv "$BUILD_DIR/$ARCHIVE.partial" "$BUILD_DIR/$ARCHIVE"
fi

if [[ "$FFMPEG_VERSION" == "7.0.2" ]]; then
  checksum=(sha256sum -c -)
  if [[ "$host_os" == "Darwin" ]]; then checksum=(shasum -a 256 -c -); fi
  (cd "$BUILD_DIR" && printf "%s  %s\n" \
    "$SOURCE_SHA256" "$ARCHIVE" | "${checksum[@]}")
fi

if [[ ! -d "$SRC_DIR" ]]; then
  echo "==> Extracting..."
  tar xf "$BUILD_DIR/$ARCHIVE" -C "$BUILD_DIR"
fi

# ── Configure (LGPL, remux-only) ─────────────────────────────
cd "$SRC_DIR"
echo "==> Configuring ffmpeg (LGPL, remux-only)..."

EXTRA_CFLAGS="-Os"
EXTRA_LDFLAGS=()
ARCH_FLAGS=()
if [[ "$ffmpeg_name" == "ffmpeg.exe" ]]; then
  EXTRA_LDFLAGS+=(--extra-ldflags=-static)
fi
if [[ "$host_os" == "Darwin" ]]; then
  export MACOSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET"
  target_flags="-arch $MACOS_ARCHITECTURE -mmacosx-version-min=$MACOS_DEPLOYMENT_TARGET"
  EXTRA_CFLAGS="$EXTRA_CFLAGS $target_flags"
  EXTRA_LDFLAGS+=("--extra-ldflags=$target_flags")
  ARCH_FLAGS+=(--arch="$ffmpeg_arch")
  if [[ "$(uname -m)" != "$MACOS_ARCHITECTURE" ]]; then
    ARCH_FLAGS+=(--enable-cross-compile --target-os=darwin)
  fi
fi

# We only use remux/extract operations, for example:
#   ffmpeg -i input.mkv -c copy -movflags +faststart output.mp4
#   ffmpeg -i input.mkv -c copy -movflags +faststart output.mov
#   ffmpeg -i input.mkv -map 0:v -map 0:a:N -c copy output.mp4
#
# Required components:
#   demuxer  = matroska (MKV input)
#   muxer    = mp4/mov/ipod (MP4/MOV output), matroska (MKV pass-through)
#   protocol = file
#   parser   = h264, hevc, aac (stream header analysis during copy)
#   bsf      = aac_adtstoasc (AAC container format conversion MKV→MP4)

./configure \
  --prefix="$INSTALL_DIR" \
  --disable-gpl \
  --disable-nonfree \
  --disable-autodetect \
  --disable-doc \
  --disable-htmlpages \
  --disable-manpages \
  --disable-podpages \
  --disable-txtpages \
  --disable-ffplay \
  --disable-ffprobe \
  --disable-network \
  --disable-encoders \
  --disable-decoders \
  --disable-hwaccels \
  --disable-muxers \
  --disable-demuxers \
  --disable-filters \
  --disable-bsfs \
  --disable-parsers \
  --disable-indevs \
  --disable-outdevs \
  --disable-protocols \
  --disable-avdevice \
  --disable-postproc \
  --enable-small \
  --enable-demuxer=matroska,mov \
  --enable-muxer=mp4,matroska,mov,ipod \
  --enable-protocol=file,pipe \
  --enable-bsf=aac_adtstoasc,h264_mp4toannexb,hevc_mp4toannexb \
  --enable-parser=h264,hevc,aac,opus,vorbis \
  --cc="$ffmpeg_cc" \
  --extra-cflags="$EXTRA_CFLAGS" \
  ${EXTRA_LDFLAGS[@]+"${EXTRA_LDFLAGS[@]}"} \
  ${ARCH_FLAGS[@]+"${ARCH_FLAGS[@]}"}

# ── Build ────────────────────────────────────────────────────
NPROC="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
echo "==> Building (${NPROC} threads)..."
make -j"$NPROC"

echo "==> Installing..."
make install

FFMPEG_BIN="$INSTALL_DIR/bin/$ffmpeg_name"
test -x "$FFMPEG_BIN"

# ── Code-sign (macOS Gatekeeper) ─────────────────────────────
if [[ "$host_os" == "Darwin" ]]; then
  codesign --force --sign - "$FFMPEG_BIN" 2>/dev/null || true
fi

# ── Summary ──────────────────────────────────────────────────
echo ""
echo "========================================"
echo "  LGPL ffmpeg built successfully"
echo "  Binary : $FFMPEG_BIN"
echo "  Size   : $(du -h "$FFMPEG_BIN" | cut -f1)"
echo "  License: LGPL-2.1 (no GPL components)"
echo "========================================"
echo ""
if [[ "$host_os" == "Darwin" ]]; then
  echo "Run install-macos-dev.sh to bundle into the plugin."
fi
