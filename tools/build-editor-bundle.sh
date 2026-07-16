#!/usr/bin/env bash
set -euo pipefail

# Build the self-contained browser editor bundle and vendor it into a site's
# editor page, under a version-stamped directory. The .bin-style hand-copy this
# replaces was the standing automation debt; "update" now means "run this".
#
# Usage: tools/build-editor-bundle.sh <dest-editor-dir>
#   e.g. tools/build-editor-bundle.sh ~/Developer/frothy-site/static/test/editor

dest_root="${1:?usage: build-editor-bundle.sh <dest-editor-dir>}"
here="$(cd "$(dirname "$0")/.." && pwd)"

version="$(node -p "require('$here/libs/frothy-editor/package.json').version")"
npm --prefix "$here/libs/frothy-editor" run build >/dev/null

dest="$dest_root/vendor/frothy-editor/$version"
mkdir -p "$dest"
cp "$here/libs/frothy-editor/dist/browser/index.js" "$dest/index.js"
cp "$here/libs/frothy-editor/fixtures/project-document-v1.json" "$dest/project-document-v1.json"
echo "vendored @frothy/editor $version -> $dest/index.js"
# The import path is version-pinned on purpose (a bump is a conscious act), so
# point the page at the new bundle and drop the old dir:
echo "  next: import from ./vendor/frothy-editor/$version/index.js and remove any older vendored dir"
