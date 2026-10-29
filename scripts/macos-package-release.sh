#!/bin/bash
# Builds a self-contained macOS (Apple Silicon) release zip: plugin, Metal worker, a minimal LGPL FFmpeg (no Homebrew
# dependency), the pinned tiny + Silero VAD models, the settings extension, and install/uninstall scripts.
# Usage: scripts/macos-package-release.sh            -> dist/vlc-whisper-<version>-macos-arm64.zip
# Env:   FFMPEG_VERSION (default 8.1.1), MACOS_MIN (default 13.0), JOBS (default: all cores)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FFMPEG_VERSION="${FFMPEG_VERSION:-8.1.1}"
MACOS_MIN="${MACOS_MIN:-13.0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
VERSION="$(sed -n 's/^project(vlc_whisper VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
WORK="$ROOT/build/macos-package"
FF_SRC="$WORK/ffmpeg-$FFMPEG_VERSION"
FF_PREFIX="$WORK/ffmpeg-prefix"
BUILD="$WORK/cmake"
NAME="vlc-whisper-$VERSION-macos-arm64"
STAGE="$WORK/stage/$NAME"
PAYLOAD="$STAGE/payload"  # mirrors VLC.app/Contents/MacOS
BUNDLED_LIB_DIR="lib/vlc-whisper"

[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || { echo "error: run on an Apple Silicon Mac" >&2; exit 1; }
mkdir -p "$WORK" "$ROOT/dist"

# 1. Minimal shared LGPL FFmpeg: demux + decode only, no external libraries beyond the system zlib, @rpath install
#    names so the worker loads the bundled copies from its own lib/vlc-whisper directory.
if [ ! -f "$FF_PREFIX/lib/libavformat.dylib" ]; then
  if [ ! -d "$FF_SRC" ]; then
    /usr/bin/curl -fL --retry 3 -o "$WORK/ffmpeg.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
    tar -xJf "$WORK/ffmpeg.tar.xz" -C "$WORK"
  fi
  (cd "$FF_SRC" && ./configure --prefix="$FF_PREFIX" --arch=arm64 --cc=/usr/bin/clang \
      --enable-shared --disable-static --install-name-dir=@rpath \
      --extra-cflags="-mmacosx-version-min=$MACOS_MIN" --extra-ldflags="-mmacosx-version-min=$MACOS_MIN" \
      --disable-programs --disable-doc --disable-network --disable-autodetect --enable-zlib \
      --disable-avdevice --disable-avfilter --disable-swscale \
      --disable-encoders --disable-muxers --disable-filters --disable-devices --disable-hwaccels \
    && make -j"$JOBS" && make install)
fi

# 2. Plugin + worker against that FFmpeg, targeting MACOS_MIN, installed into the staging payload. GGML_NATIVE=OFF:
#    -mcpu=native on the build Mac (e.g. M4: +i8mm+sme) would crash older Apple Silicon; the compiler's arm64 macOS
#    default is the M1 baseline (dotprod, fp16 SIMD), which every Apple Silicon Mac supports.
rm -rf "$STAGE"
PKG_CONFIG_PATH="$FF_PREFIX/lib/pkgconfig" cmake --preset macos-arm64-release -B "$BUILD" -DGGML_NATIVE=OFF \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_MIN" -DCMAKE_INSTALL_PREFIX="$PAYLOAD" \
  -DCMAKE_INSTALL_RPATH="@executable_path/$BUNDLED_LIB_DIR" -DVW_PROVISION_MODELS=ON
cmake --build "$BUILD" -j"$JOBS"
cmake --build "$BUILD" --target provision_models
cmake --install "$BUILD"

# 3. Bundle the FFmpeg dylibs the worker actually references (and their own @rpath dependencies).
mkdir -p "$PAYLOAD/$BUNDLED_LIB_DIR"
deps() { otool -L "$1" | awk 'NR > 1 && $1 ~ /^@rpath\/lib(av|sw)/ { sub("@rpath/", "", $1); print $1 }'; }
queue=("$PAYLOAD/vlc-whisper-worker")
while [ ${#queue[@]} -gt 0 ]; do
  bin="${queue[0]}"; queue=("${queue[@]:1}")
  for lib in $(deps "$bin"); do
    if [ ! -f "$PAYLOAD/$BUNDLED_LIB_DIR/$lib" ]; then
      cp "$FF_PREFIX/lib/$lib" "$PAYLOAD/$BUNDLED_LIB_DIR/$lib"
      chmod 644 "$PAYLOAD/$BUNDLED_LIB_DIR/$lib"
      queue+=("$PAYLOAD/$BUNDLED_LIB_DIR/$lib")
    fi
  done
done
for lib in "$PAYLOAD/$BUNDLED_LIB_DIR"/*.dylib; do
  # Each bundled dylib resolves its siblings through the loading executable's rpath; keep no build-tree rpaths.
  for rp in $(otool -l "$lib" | awk '/LC_RPATH/ { getline; getline; print $2 }'); do install_name_tool -delete_rpath "$rp" "$lib"; done
  codesign --force --sign - "$lib"
done

# 4. Refuse to ship anything that still points at Homebrew or the build tree.
for bin in "$PAYLOAD/vlc-whisper-worker" "$PAYLOAD/plugins/libvlc_whisper_plugin.dylib" "$PAYLOAD/$BUNDLED_LIB_DIR"/*.dylib; do
  if otool -L "$bin" | tail -n +2 | grep -Ev '^\s+(/usr/lib/|/System/Library/|@rpath/lib(av|sw)|@rpath/libvlc_whisper_plugin)' ; then
    echo "error: $bin has a non-system dependency (above)" >&2; exit 1
  fi
  if otool -l "$bin" | grep -A2 LC_RPATH | grep -E "path (/opt/|$ROOT)"; then
    echo "error: $bin keeps a build-machine rpath" >&2; exit 1
  fi
done
codesign --verify "$PAYLOAD/vlc-whisper-worker" "$PAYLOAD/plugins/libvlc_whisper_plugin.dylib"
if grep -Eq -- '-mcpu=native|\+sme|\+i8mm|\+sve' "$BUILD/build.ninja" "$BUILD"/worker/third_party/whisper.cpp/ggml/src/CMakeFiles/*/flags.make 2>/dev/null \
   || grep -Eq -- '-mcpu=(native|apple-m[2-9])|\+sme|\+i8mm' "$BUILD/compile_commands.json" 2>/dev/null; then
  echo "error: ggml was compiled for the build machine's CPU, not the Apple Silicon baseline" >&2; exit 1
fi

# 5. Installer, uninstaller, menu helper, docs, FFmpeg licence + source pointer.
cp "$ROOT/scripts/macos-install.sh" "$STAGE/install.sh"
cp "$ROOT/scripts/macos-uninstall.sh" "$STAGE/uninstall.sh"
cp "$ROOT/scripts/macos-enable-menu.sh" "$STAGE/enable-menu.sh"
chmod 755 "$STAGE"/*.sh
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/"
cp "$FF_SRC/COPYING.LGPLv2.1" "$PAYLOAD/share/doc/vlc-whisper/FFmpeg-COPYING.LGPLv2.1"
cat > "$PAYLOAD/share/doc/vlc-whisper/FFmpeg-SOURCE.txt" <<TXT
The bundled libavformat, libavcodec, libavutil and libswresample are unmodified FFmpeg $FFMPEG_VERSION, built as
shared LGPL v2.1+ libraries (no --enable-gpl / --enable-nonfree). Source: https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz
Build script: scripts/macos-package-release.sh in https://github.com/jleppert/vlc-whisper
TXT
cat > "$STAGE/README.txt" <<TXT
VLC-Whisper $VERSION for macOS (Apple Silicon)

Install (quit VLC first):   ./install.sh                      (or ./install.sh /path/to/VLC.app)
Uninstall:                  ./uninstall.sh

Then open VLC and choose Subtitles > Whisper AI Captions. More models (small, large-v3-turbo, ...) can be
selected in VLC media player > Extensions > VLC-Whisper Settings. Details: https://github.com/jleppert/vlc-whisper#macos-apple-silicon
TXT

# 6. Zip (ditto keeps the models symlink and code signatures).
rm -f "$ROOT/dist/$NAME.zip"
(cd "$WORK/stage" && ditto -c -k --norsrc --noextattr --noacl --keepParent "$NAME" "$ROOT/dist/$NAME.zip")
shasum -a 256 "$ROOT/dist/$NAME.zip" | tee "$ROOT/dist/$NAME.zip.sha256"
