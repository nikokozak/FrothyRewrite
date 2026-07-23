#!/bin/sh
# Single owner of the release name. Every surface that embeds or reports a
# release -- Makefile (FR_RELEASE), the ESP-IDF build, the flasher bundle
# manifest -- takes this value, so firmware and metadata cannot disagree.
cd "$(dirname "$0")/.." || exit 1
git describe --tags --always --dirty 2>/dev/null || echo dev
