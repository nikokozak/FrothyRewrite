#!/usr/bin/env bash
#
# Fetch and install ESP-IDF for Frothy development.
# Installs to $FROTH_HOME/sdk/esp-idf/ when FROTH_HOME is set, and otherwise
# uses ~/.froth/sdk/esp-idf/. No sudo. No writes outside that tree.
#
# Usage:
#   ./tools/setup-esp-idf.sh           # install (skip if already present)
#   ./tools/setup-esp-idf.sh --force   # reinstall from scratch

set -eu

ESP_IDF_VERSION="v5.5"
FROTH_HOME_DIR="${FROTH_HOME:-$HOME/.froth}"
FROTH_SDK_DIR="$FROTH_HOME_DIR/sdk"
IDF_INSTALL_DIR="$FROTH_SDK_DIR/esp-idf"
IDF_READY_MARKER=".froth-install-complete"

usage() {
  cat <<'EOF'
usage:
  ./tools/setup-esp-idf.sh
  ./tools/setup-esp-idf.sh --force
EOF
}

FORCE=0
case "${1-}" in
  "")
    ;;
  --force)
    FORCE=1
    shift
    ;;
  -h|--help)
    usage
    exit 0
    ;;
  *)
    usage >&2
    exit 1
    ;;
esac

if [ "$#" -ne 0 ]; then
  usage >&2
  exit 1
fi

if ! command -v git >/dev/null 2>&1; then
  echo "error: git is not installed."
  exit 1
fi

if [ -d "$IDF_INSTALL_DIR" ]; then
  if [ "$FORCE" -eq 0 ] && [ -f "$IDF_INSTALL_DIR/$IDF_READY_MARKER" ]; then
    echo "ESP-IDF already installed at $IDF_INSTALL_DIR"
    echo "Run with --force to reinstall."
    echo ""
    echo "To activate:  source $IDF_INSTALL_DIR/export.sh"
    exit 0
  fi
  if [ "$FORCE" -eq 0 ]; then
    echo "Found incomplete ESP-IDF install at $IDF_INSTALL_DIR; reinstalling."
  else
    echo "Replacing existing install at $IDF_INSTALL_DIR..."
  fi
fi

mkdir -p "$FROTH_SDK_DIR"
STAGE_DIR="$(mktemp -d "$FROTH_SDK_DIR/.esp-idf-stage.XXXXXX")"
STAGE_INSTALL_DIR="$STAGE_DIR/esp-idf"
BACKUP_DIR="$FROTH_SDK_DIR/.esp-idf-backup.$$"

cleanup() {
  rm -rf "$STAGE_DIR"
}
trap cleanup EXIT

echo "Cloning ESP-IDF $ESP_IDF_VERSION..."
git clone \
  --branch "$ESP_IDF_VERSION" \
  --recursive \
  --depth 1 \
  --shallow-submodules \
  https://github.com/espressif/esp-idf.git \
  "$STAGE_INSTALL_DIR"

if [ -d "$IDF_INSTALL_DIR" ]; then
  rm -rf "$BACKUP_DIR"
  mv "$IDF_INSTALL_DIR" "$BACKUP_DIR"
fi

if ! mv "$STAGE_INSTALL_DIR" "$IDF_INSTALL_DIR"; then
  if [ -d "$BACKUP_DIR" ]; then
    mv "$BACKUP_DIR" "$IDF_INSTALL_DIR"
  fi
  echo "error: failed to activate ESP-IDF install at $IDF_INSTALL_DIR"
  exit 1
fi

restore_backup() {
  rm -rf "$IDF_INSTALL_DIR"
  if [ -d "$BACKUP_DIR" ]; then
    mv "$BACKUP_DIR" "$IDF_INSTALL_DIR"
  fi
}

echo "Installing ESP-IDF toolchain (esp32 target only)..."
if ! "$IDF_INSTALL_DIR/install.sh" esp32; then
  restore_backup
  echo "error: failed to install ESP-IDF toolchain in $IDF_INSTALL_DIR"
  exit 1
fi

touch "$IDF_INSTALL_DIR/$IDF_READY_MARKER"
rm -rf "$BACKUP_DIR"

echo ""
echo "Done. To activate ESP-IDF in your current shell, run:"
echo ""
echo "  source $IDF_INSTALL_DIR/export.sh"
echo ""
echo "Then verify the toolchain:"
echo ""
echo "  frothy doctor"
echo ""
echo "From here you can:"
echo ""
echo "  * Flash Frothy onto a board. This builds the firmware and writes it"
echo "    over serial; you pass the board and (if needed) the serial port."
echo "    See the options with:"
echo ""
echo "        frothy flash --help"
echo ""
echo "  * Try the language right now on your computer, no board needed."
echo "    Build the host REPL and run it, then type Frothy at the > prompt:"
echo ""
echo "        make frothy && ./frothy"
echo ""
