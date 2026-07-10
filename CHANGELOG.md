# Changelog

All notable changes to Frothy are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/); versions are the `vX.Y.Z` git
tags described in the "Releasing" section of CONTRIBUTING.md.

## [Unreleased]

### Changed

- **Interrupting a running program is no longer reported as an error.**
  Stopping a loop with Ctrl-C or the boot button showed
  `error: interrupted (10)`; it now prints a calm `ok — interrupted`. The
  change is device-side, so the web editor and VS Code stop flagging a
  deliberate stop as a failure too.

### Fixed

- **No more `esp_mmu_map` error log on save (ESP32).** With a saved image
  already mounted, every `save` printed a benign but alarming
  `E (…) esp_mmu_map: paddr block is mapped already`. Save re-mapped the slot
  it had already mapped; it now reuses the existing mapping.

## [0.1.1] - 2026-07-10

### Fixed

- **`save` no longer corrupts when a word matches a built-in.** Saving a word
  whose bytecode is byte-identical to a built-in — for example `x is led.on`, or
  a blink word that calls `led.on` — aborted with `corrupt data (11)`. The code
  encoder's deduplication scanned base-image codes that the bind resolver skips,
  so such a word was deduped away and then could not be resolved. The two scans
  now agree, and the word gets its own record.

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
