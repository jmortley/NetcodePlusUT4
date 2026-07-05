#!/usr/bin/env bash
# Usage: ./update-hub.sh /path/to/UnrealTournament/UnrealTournament/Plugins
set -euo pipefail
PLUGINS_DIR="${1:?usage: update-hub.sh <.../UnrealTournament/Plugins>}"
MANIFEST="https://github.com/jmortley/netcodeplus-launcher/releases/download/updates-latest/manifest.json"

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -fsSL "$MANIFEST" -o "$tmp/manifest.json"

read -r URL SHA VER < <(python3 - "$tmp/manifest.json" <<'PY'
import json,sys
p=json.load(open(sys.argv[1]))["channels"]["stable"]["plugin"]
print(p["url"], p["sha256"], p["version"])
PY
)
echo "Latest NetcodePlus build: $VER"
curl -fsSL "$URL" -o "$tmp/ncp.zip"
echo "${SHA}  ${tmp}/ncp.zip" | sha256sum -c -

dest="$PLUGINS_DIR/NetcodePlus"
# Backups must live OUTSIDE Plugins/: UE4 scans everything under Plugins/ for
# .uplugin files, and on Linux directory order is arbitrary — a leftover
# NetcodePlus.bak.* INSIDE Plugins/ can be discovered first, shadowing the real
# plugin ("second location will be ignored") so the server silently runs the
# OLD DLL. Same failure class as the launcher's client-side .old leftovers.
backup_root="$(dirname "$PLUGINS_DIR")/PluginBackups"
mkdir -p "$backup_root"

# Sweep legacy in-Plugins backups left by older versions of this script.
for old in "$PLUGINS_DIR"/NetcodePlus.bak.*; do
  [ -d "$old" ] && mv "$old" "$backup_root/" && echo "Relocated legacy backup: $(basename "$old")"
done

[ -d "$dest" ] && mv "$dest" "$backup_root/NetcodePlus.bak.$(date +%Y%m%d%H%M%S)"
mkdir -p "$dest"
unzip -q "$tmp/ncp.zip" -d "$dest"

# Keep the two newest backups, prune the rest.
ls -1dt "$backup_root"/NetcodePlus.bak.* 2>/dev/null | tail -n +3 | xargs -r rm -rf

echo "Installed build $VER -> $dest . Restart the server."
