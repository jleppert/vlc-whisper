#!/bin/sh
# Registers (or removes) the vlc-whisper Subtitles-menu toggle as a VLC extra interface in the per-user vlcrc.
# Usage: macos-enable-menu.sh [--disable]
set -eu
MODULE="vlc_whisper_menu"

# A running VLC rewrites vlcrc from its in-memory settings whenever preferences are saved, silently undoing this edit.
if pgrep -x VLC >/dev/null 2>&1; then
  echo "vlc-whisper: quit VLC first; a running VLC overwrites vlcrc when it saves preferences" >&2
  exit 1
fi
RC="$HOME/Library/Preferences/org.videolan.vlc/vlcrc"
mkdir -p "$(dirname "$RC")"
[ -f "$RC" ] || printf '[core]\n' > "$RC"

current="$(sed -n 's/^extraintf=//p' "$RC" | head -n 1)"
if [ "${1:-}" = "--disable" ]; then
  new="$(printf '%s' "$current" | tr ':' '\n' | grep -vx "$MODULE" | paste -sd ':' - || true)"
else
  case ":$current:" in
    *":$MODULE:"*) new="$current" ;;
    "::") new="$MODULE" ;;
    *) new="$current:$MODULE" ;;
  esac
fi

tmp="$(mktemp)"
if grep -q '^#\{0,1\}extraintf=' "$RC"; then
  # Replace the first (commented or active) extraintf line in place; drop any further active duplicates.
  awk -v val="$new" '
    /^#?extraintf=/ && !done { print "extraintf=" val; done = 1; next }
    /^extraintf=/ { next }
    { print }' "$RC" > "$tmp"
else
  cat "$RC" > "$tmp"; printf 'extraintf=%s\n' "$new" >> "$tmp"
fi
cat "$tmp" > "$RC"; rm -f "$tmp"
echo "vlcrc extraintf=$new (restart VLC to apply)"
