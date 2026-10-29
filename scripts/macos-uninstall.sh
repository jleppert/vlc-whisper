#!/bin/bash
# Removes VLC-Whisper from VLC.app (shipped as uninstall.sh in the release zip). Downloaded models in
# ~/.local/share/vlc-whisper are kept; delete that folder to reclaim the space.
# Usage: ./uninstall.sh [/path/to/VLC.app]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
VLC_APP="${1:-/Applications/VLC.app}"
DEST="${VLC_APP%/}/Contents/MacOS"
[ -d "$DEST" ] || { echo "vlc-whisper: VLC not found at $VLC_APP" >&2; exit 1; }
pgrep -x VLC >/dev/null 2>&1 && { echo "vlc-whisper: quit VLC first" >&2; exit 1; }

SUDO=""
[ -w "$DEST" ] && [ -w "$DEST/plugins" ] || SUDO="sudo"
$SUDO rm -rf "$DEST/plugins/libvlc_whisper_plugin.dylib" "$DEST/vlc-whisper-worker" "$DEST/vlc-whisper-worker-cpu" \
  "$DEST/lib/vlc-whisper" "$DEST/models" "$DEST/share/models" \
  "$DEST/share/lua/extensions/vlc_whisper_settings.lua" "$DEST/share/doc/vlc-whisper"
if [ -x "$HERE/enable-menu.sh" ]; then
  "$HERE/enable-menu.sh" --disable
fi
# The audio filter may have been left enabled in vlcrc by the menu toggle; VLC ignores unknown filters, but tidy it.
RC="$HOME/Library/Preferences/org.videolan.vlc/vlcrc"
if [ -f "$RC" ] && grep -q '^audio-filter=.*vlc_whisper' "$RC"; then
  sed -i '' -e 's/^audio-filter=\(.*\)$/audio-filter=:\1:/' -e '/^audio-filter=/s/:vlc_whisper:/:/g' \
    -e 's/^audio-filter=:*\(.*[^:]\):*$/audio-filter=\1/' -e 's/^audio-filter=:*$/audio-filter=/' "$RC"
fi
echo "VLC-Whisper removed from $VLC_APP"
