#!/bin/bash
# Installs a VLC-Whisper macOS release into VLC.app (shipped as install.sh in the release zip).
# Usage: ./install.sh [/path/to/VLC.app] [--no-menu]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
VLC_APP="/Applications/VLC.app"
ENABLE_MENU=1
for arg in "$@"; do
  case "$arg" in
    --no-menu) ENABLE_MENU=0 ;;
    -h|--help) sed -n '2,3p' "$0"; exit 0 ;;
    *) VLC_APP="${arg%/}" ;;
  esac
done
DEST="$VLC_APP/Contents/MacOS"
die() { echo "vlc-whisper: $*" >&2; exit 1; }

[ -d "$HERE/payload" ] || die "payload/ not found next to install.sh; run it from the unzipped release folder"
[ -x "$DEST/VLC" ] || die "VLC not found at $VLC_APP (download it from https://www.videolan.org, or pass its path)"
[ "$(uname -m)" = arm64 ] || die "this build is for Apple Silicon Macs only"
lipo -archs "$DEST/VLC" 2>/dev/null | grep -qw arm64 || die "$VLC_APP is not an Apple Silicon (arm64/universal) VLC build"
VLC_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$VLC_APP/Contents/Info.plist" 2>/dev/null || true)"
case "$VLC_VERSION" in 3.0.*) ;; *) die "VLC 3.0.x is required (found '${VLC_VERSION:-unknown}')" ;; esac
pgrep -x VLC >/dev/null 2>&1 && die "quit VLC first, then run this again"
# Gatekeeper assesses an app on its first launch. Adding files breaks VLC.app's sealed signature, so a VLC that has never
# been opened would then be reported as "damaged"; one that macOS has already approved keeps launching normally.
if [ ! -e "$HOME/Library/Preferences/org.videolan.vlc" ] && [ ! -e "$HOME/Library/Preferences/org.videolan.vlc.plist" ]; then
  die "open VLC once (so macOS approves it), quit it, then run this again"
fi

SUDO=""
[ -w "$DEST" ] && [ -w "$DEST/plugins" ] || { echo "Administrator rights are needed to write into $VLC_APP"; SUDO="sudo"; }

# Files from a downloaded zip carry the quarantine flag; Gatekeeper would refuse to load the unsigned plugin/worker.
xattr -dr com.apple.quarantine "$HERE" 2>/dev/null || true

# A stale models symlink or older plugin copy would shadow the new payload.
$SUDO rm -f "$DEST/models"
$SUDO ditto "$HERE/payload" "$DEST"
$SUDO xattr -dr com.apple.quarantine "$DEST/plugins/libvlc_whisper_plugin.dylib" "$DEST/vlc-whisper-worker" \
  "$DEST/lib/vlc-whisper" "$DEST/share/models" 2>/dev/null || true
echo "Installed VLC-Whisper into $DEST (VLC $VLC_VERSION)"

if [ "$ENABLE_MENU" = 1 ]; then
  "$HERE/enable-menu.sh"
fi

cat <<TXT

Done. Open VLC, play something with speech, and choose Subtitles > Whisper AI Captions.
The bundled model is Whisper tiny; larger ones (small, large-v3-turbo, ...) can be picked in the
VLC-Whisper Settings extension. Updating VLC replaces VLC.app, so run this installer again afterwards.
TXT
