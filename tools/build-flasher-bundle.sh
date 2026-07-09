#!/usr/bin/env bash
set -euo pipefail

# Build the merged ESP32 flasher binary and vendor it into a site's flasher page,
# stamping the build version into the page's manifest. This is the local flow the
# release workflow (.github/workflows/release.yml) mirrors on a tag; see the
# "Releasing" section of CONTRIBUTING.md. The .bin has embedded build timestamps,
# so it is never byte-identical between builds — "update" means "re-run this",
# never diff bytes.
#
# Prereq: ESP-IDF installed once via tools/setup-esp-idf.sh.
# Usage: tools/build-flasher-bundle.sh <dest-flash-dir>
#   e.g. tools/build-flasher-bundle.sh ~/Developer/frothy-site/static/test/flash

dest_root="${1:?usage: build-flasher-bundle.sh <dest-flash-dir>}"
here="$(cd "$(dirname "$0")/.." && pwd)"

# Validate the destination before the multi-minute build, so a wrong path fails
# fast instead of after a full firmware rebuild.
manifest="$dest_root/firmware/manifest.json"
[ -f "$manifest" ] || { echo "no manifest at $manifest (is the dest a flasher page dir?)" >&2; exit 1; }

version="$(git -C "$here" describe --tags --always --dirty 2>/dev/null || echo unknown)"

# web-bins runs the ESP-IDF build; source the SDK so idf.py/esptool are on PATH.
if [ -f "$HOME/.froth/sdk/esp-idf/export.sh" ]; then
  # shellcheck disable=SC1091
  . "$HOME/.froth/sdk/esp-idf/export.sh" >/dev/null
fi
make -C "$here" web-bins

bin="$here/web/flash/firmware/esp32_devkit_v1-esp32_plain.bin"
cp "$bin" "$dest_root/firmware/"

# Stamp the build version onto every manifest entry, leaving the rest intact.
node -e '
const fs = require("fs");
const [file, version] = process.argv.slice(1);
const data = JSON.parse(fs.readFileSync(file, "utf8"));
const entries = Array.isArray(data) ? data : [data];
for (const e of entries) e.version = version;
fs.writeFileSync(file, JSON.stringify(entries, null, 2) + "\n");
' "$manifest" "$version"

echo "vendored flasher bin @ $version -> $dest_root/firmware/"
echo "  manifest stamped: version=$version"
