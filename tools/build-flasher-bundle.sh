#!/usr/bin/env bash
set -euo pipefail

# Build the ESP32 firmware segments and write the browser-flasher manifest.
# Addresses come only from ESP-IDF's generated flasher_args.json, so the bundle
# contains no bytes from gaps such as the NVS partition.
#
# Prereq: ESP-IDF installed once via tools/setup-esp-idf.sh.
# Usage: tools/build-flasher-bundle.sh <dest-firmware-dir>
#   e.g. tools/build-flasher-bundle.sh ~/Developer/frothy-site/static/test/flash/firmware

dest="${1:?usage: build-flasher-bundle.sh <dest-firmware-dir>}"
here="$(cd "$(dirname "$0")/.." && pwd)"
board="esp32_devkit_v1"
board_json="$here/boards/$board/board.json"

[ -f "$board_json" ] || { echo "missing board manifest: $board_json" >&2; exit 1; }
profile="$(node -e '
const board = require(process.argv[1]);
if (typeof board.profile !== "string" || !board.profile) throw new Error("board profile missing");
process.stdout.write(board.profile);
' "$board_json")"

version="$(git -C "$here" describe --tags --always --dirty 2>/dev/null || echo unknown)"
build_dir="$here/build/$board"
flasher_args="$build_dir/flasher_args.json"

make -C "$here" BOARD="$board" PROFILE="$profile" artifacts
[ -f "$flasher_args" ] || { echo "missing ESP-IDF flasher arguments: $flasher_args" >&2; exit 1; }

node - "$flasher_args" "$board_json" "$build_dir" "$dest" "$version" "$board" <<'NODE'
const fs = require("fs");
const path = require("path");

const [argsFile, boardFile, buildDir, destination, version, boardId] = process.argv.slice(2);
const args = JSON.parse(fs.readFileSync(argsFile, "utf8"));
const board = JSON.parse(fs.readFileSync(boardFile, "utf8"));
if (typeof board.name !== "string" || !board.name) throw new Error("board name missing");
if (typeof board.profile !== "string" || !board.profile) throw new Error("board profile missing");
if (!args.flash_files || typeof args.flash_files !== "object" || Array.isArray(args.flash_files)) {
  throw new Error("flasher_args.json has no flash_files object");
}

const prefix = `${boardId}-${board.profile}-`;
const buildRoot = path.resolve(buildDir);
const addresses = new Set();
const segments = Object.entries(args.flash_files).map(([encodedAddress, relativeFile]) => {
  const address = Number(encodedAddress);
  if (!Number.isSafeInteger(address) || address < 0) {
    throw new Error(`invalid flash address: ${encodedAddress}`);
  }
  if (addresses.has(address)) throw new Error(`duplicate flash address: ${encodedAddress}`);
  addresses.add(address);
  if (typeof relativeFile !== "string" || !relativeFile) {
    throw new Error(`invalid flash file at ${encodedAddress}`);
  }
  const source = path.resolve(buildRoot, relativeFile);
  if (!source.startsWith(buildRoot + path.sep) || !fs.statSync(source).isFile()) {
    throw new Error(`flash file escapes build directory or is missing: ${relativeFile}`);
  }
  const file = `${prefix}0x${address.toString(16).padStart(8, "0")}.bin`;
  return { address, file, source };
}).sort((a, b) => a.address - b.address);

if (segments.length === 0) throw new Error("flasher_args.json lists no flash files");
fs.mkdirSync(destination, { recursive: true });
for (const file of fs.readdirSync(destination)) {
  const generatedSegment = file.startsWith(prefix) && file.endsWith(".bin");
  const legacyMergedImage = file === `${boardId}-${board.profile}.bin`;
  if (generatedSegment || legacyMergedImage) fs.unlinkSync(path.join(destination, file));
}
for (const segment of segments) {
  fs.copyFileSync(segment.source, path.join(destination, segment.file));
}
const manifest = [{
  board: boardId,
  profile: board.profile,
  label: board.name,
  version,
  segments: segments.map(({ address, file }) => ({ address, file })),
}];
fs.writeFileSync(path.join(destination, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
console.log(`built ${segments.length} flasher segments @ ${version} -> ${destination}`);
NODE
