#!/usr/bin/env bash
set -euo pipefail

# Build every official ESP-IDF board and write one browser-flasher bundle.
# Addresses come only from each ESP-IDF build's flasher_args.json, so the
# bundle contains no bytes from gaps such as the NVS partition.
#
# Prereq: ESP-IDF installed once via tools/setup-esp-idf.sh.
# Usage: tools/build-flasher-bundle.sh <dest-firmware-dir>
#   e.g. tools/build-flasher-bundle.sh ~/Developer/frothy-site/static/test/flash/firmware

dest="${1:?usage: build-flasher-bundle.sh <dest-firmware-dir>}"
here="$(cd "$(dirname "$0")/.." && pwd)"
version="$(git -C "$here" describe --tags --always --dirty 2>/dev/null || echo unknown)"
repl_version="$(node -p "require('$here/libs/frothy-repl/package.json').version")"

npm --prefix "$here/libs/frothy-repl" run build >/dev/null
repl_dest="$(dirname "$dest")/vendor/frothy-repl/$repl_version"

node - "$here" "$dest" "$version" <<'NODE'
const childProcess = require("child_process");
const fs = require("fs");
const path = require("path");

const [root, destinationArg, version] = process.argv.slice(2);
const boardsDir = path.join(root, "boards");
const destination = path.resolve(destinationArg);
const generatedSegment = /^.+-.+-0x[0-9a-fA-F]+\.bin$/;
const bundleIdPattern = /^[a-z0-9_]+$/;
const outputFiles = new Set();
const builds = [];

for (const entry of fs.readdirSync(boardsDir, { withFileTypes: true })
  .filter((candidate) => candidate.isDirectory())
  .sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0)) {
  const boardId = entry.name;
  const boardFile = path.join(boardsDir, boardId, "board.json");
  if (!fs.existsSync(boardFile)) continue;

  const board = JSON.parse(fs.readFileSync(boardFile, "utf8"));
  if (board.target !== "esp-idf") continue;
  if (!bundleIdPattern.test(boardId)) throw new Error(`invalid board id: ${boardId}`);
  if (typeof board.name !== "string" || !board.name) {
    throw new Error(`${boardId}: board name missing`);
  }
  if (typeof board.profile !== "string" || !board.profile) {
    throw new Error(`${boardId}: board profile missing`);
  }
  if (typeof board.chip !== "string" || !/^[a-z0-9]+$/.test(board.chip)) {
    throw new Error(`${boardId}: board chip missing or invalid`);
  }
  if (!bundleIdPattern.test(board.profile)) {
    throw new Error(`${boardId}: invalid board profile ${board.profile}`);
  }

  const result = childProcess.spawnSync(
    "make",
    ["-C", root, `BOARD=${boardId}`, `PROFILE=${board.profile}`, "artifacts"],
    { stdio: "inherit" },
  );
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${boardId}: firmware build failed`);

  const buildRoot = path.join(root, "build", boardId);
  const argsFile = path.join(buildRoot, "flasher_args.json");
  if (!fs.existsSync(argsFile)) {
    throw new Error(`${boardId}: missing ESP-IDF flasher arguments`);
  }
  const args = JSON.parse(fs.readFileSync(argsFile, "utf8"));
  if (!args.flash_files || typeof args.flash_files !== "object" || Array.isArray(args.flash_files)) {
    throw new Error(`${boardId}: flasher_args.json has no flash_files object`);
  }

  const addresses = new Set();
  const prefix = `${boardId}-${board.profile}-`;
  const segments = Object.entries(args.flash_files).map(([encodedAddress, relativeFile]) => {
    const address = Number(encodedAddress);
    if (!Number.isSafeInteger(address) || address < 0) {
      throw new Error(`${boardId}: invalid flash address ${encodedAddress}`);
    }
    if (addresses.has(address)) {
      throw new Error(`${boardId}: duplicate flash address ${encodedAddress}`);
    }
    addresses.add(address);
    if (typeof relativeFile !== "string" || !relativeFile) {
      throw new Error(`${boardId}: invalid flash file at ${encodedAddress}`);
    }

    const source = path.resolve(buildRoot, relativeFile);
    const sourceRelative = path.relative(buildRoot, source);
    if (sourceRelative.startsWith(`..${path.sep}`) || path.isAbsolute(sourceRelative)) {
      throw new Error(`${boardId}: flash file escapes build directory: ${relativeFile}`);
    }
    if (!fs.statSync(source).isFile()) {
      throw new Error(`${boardId}: flash file is missing: ${relativeFile}`);
    }

    const file = `${prefix}0x${address.toString(16).padStart(8, "0")}.bin`;
    if (outputFiles.has(file)) throw new Error(`duplicate output file: ${file}`);
    outputFiles.add(file);
    return { address, file, source };
  }).sort((a, b) => a.address - b.address);

  if (segments.length === 0) {
    throw new Error(`${boardId}: flasher_args.json lists no flash files`);
  }
  builds.push({ boardId, board, segments });
}

if (builds.length === 0) throw new Error("no official ESP-IDF boards found");

const manifest = builds.map(({ boardId, board, segments }) => ({
  board: boardId,
  chip: board.chip,
  profile: board.profile,
  label: board.name,
  version,
  segments: segments.map(({ address, file }) => ({ address, file })),
}));

const parent = path.dirname(destination);
const destinationName = path.basename(destination);
fs.mkdirSync(parent, { recursive: true });
const stage = fs.mkdtempSync(path.join(parent, `.${destinationName}.stage-`));
let backup = "";

try {
  if (fs.existsSync(destination)) {
    if (!fs.lstatSync(destination).isDirectory()) {
      throw new Error(`destination is not a directory: ${destination}`);
    }
    const currentLegacyImages = new Set(
      builds.map(({ boardId, board }) => `${boardId}-${board.profile}.bin`),
    );
    for (const entry of fs.readdirSync(destination, { withFileTypes: true })) {
      if (entry.name === "manifest.json" || generatedSegment.test(entry.name) ||
          currentLegacyImages.has(entry.name)) {
        continue;
      }
      fs.cpSync(path.join(destination, entry.name), path.join(stage, entry.name), {
        recursive: true,
      });
    }
  }

  for (const { segments } of builds) {
    for (const segment of segments) {
      fs.copyFileSync(segment.source, path.join(stage, segment.file));
    }
  }
  fs.writeFileSync(
    path.join(stage, "manifest.json"),
    JSON.stringify(manifest, null, 2) + "\n",
  );

  if (fs.existsSync(destination)) {
    backup = path.join(parent, `.${destinationName}.backup-${process.pid}-${Date.now()}`);
    fs.renameSync(destination, backup);
  }
  fs.renameSync(stage, destination);
  if (backup) fs.rmSync(backup, { recursive: true, force: true });
} catch (error) {
  if (backup && fs.existsSync(backup) && !fs.existsSync(destination)) {
    fs.renameSync(backup, destination);
  }
  if (fs.existsSync(stage)) fs.rmSync(stage, { recursive: true, force: true });
  throw error;
}

const segmentCount = builds.reduce((count, build) => count + build.segments.length, 0);
console.log(`built ${segmentCount} segments for ${builds.length} boards @ ${version} -> ${destination}`);
NODE

mkdir -p "$repl_dest"
cp "$here/libs/frothy-repl/dist/browser/index.js" "$repl_dest/index.js"
echo "vendored @frothy/repl $repl_version -> $repl_dest/index.js"
