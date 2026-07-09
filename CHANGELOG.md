# Changelog

All notable changes to Frothy are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/); versions are the `vX.Y.Z` git
tags described in the "Releasing" section of CONTRIBUTING.md.

## [Unreleased]

## [0.1.0] - 2026-07-09

The first tagged release. Frothy is a live language for small 32-bit
microcontrollers: flash a board, open a prompt, define words, inspect what the
device knows, and save your work back onto it.

### Language

- **Error handling** — `attempt [ ... ] rescue [ ... ]`, with `error.code` and
  `error.name` inside the rescue block. `INTERRUPTED` is never catchable.
- **Friendly diagnostics** — compile and runtime errors report a phrase and
  code; compile errors also show the offending source with a caret and a
  did-you-mean suggestion.
- **Loops and locals** — `repeat N as <name>` exposes the loop index; `here`
  declares a mutable local.
- **Records** on the device profile, plus `gpio.output` / `gpio.input` /
  `adc.percent` convenience words.

### Libraries

- **Dependencies** — a project's `frothy.toml` can depend on a Frothy library by
  git URL (with rev or branch) or by path; `frothy fetch` / `frothy build`
  resolve, fetch, and compile them. Pure-Frothy and native (C) libraries are
  both supported.

### Editors

- **Browser editor** (frothy.dev) — an examples picker, run control (interrupt,
  stop-on-first-error), autosave, grouped error diagnostics, and a
  vertical/horizontal split for the device output.
- **VS Code extension** — connect, send, inspect, and save/restore over a live
  session; `Frothy: Open Example`; correct `--` comment support.

### Project

- **Continuous integration** — host suites, an ESP32 firmware build, an
  end-to-end library fixture, and the examples battery run on every change.
- **Examples** — one-screen sketches that teach the language and double as the
  host smoke battery.
- **Release automation** — a version tag builds and attaches the web-flasher
  firmware and the VS Code extension to a GitHub release.
