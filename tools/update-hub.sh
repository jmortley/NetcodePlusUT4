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
[ -d "$dest" ] && mv "$dest" "${dest}.bak.$(date +%Y%m%d%H%M%S)"
mkdir -p "$dest"
unzip -q "$tmp/ncp.zip" -d "$dest"
echo "Installed build $VER -> $dest . Restart the server."
