# Changelog

All notable changes to Frothy are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/); versions are the `vX.Y.Z` git
tags described in the "Releasing" section of CONTRIBUTING.md.

## [Unreleased]

### Changed

- **`pwm.open` on a pin that is already open reports `busy`**, not
  `bad value`. The two failures need different fixes — a held pin wants a
  `pwm.close` or reset, an invalid pin wants different wiring — and they
  were indistinguishable at the prompt.
- **`frothy source-plan` accepts `--entry <path>`** (default `main.fr`) so
  the editor can run any project file as the include-resolution root. Entry
  paths obey the same confinement rules as include targets.

### Removed

- **Duration suffixes on integer literals** (`2s`, `500ms`, `400us`, `400ns`),
  introduced alongside digit grouping and shipped in 0.1.10. The suffix was a
  compile-time multiply with no duration type behind it, so it looked checked
  but was not: `wait: 50ns` meant 50 milliseconds, and `2s` in a
  nanosecond-unit call meant 2 microseconds. A unit now lives where it always
  did — in the word's contract and, when a word has a non-default unit, in its
  name (`pulse.duration-ns`, `trace.delta-ns`). A digit-led token with a
  trailing suffix falls back to being a name, like any other non-integer
  token. Underscore digit grouping (`1_000_000`) is unchanged.

## [0.1.10] - 2026-07-23

### Fixed

- **Ctrl-C reaches a running program even behind buffered input.** The
  safe-point interrupt poll re-examined one saved typeahead byte forever, so
  any character that arrived before a Ctrl-C — a host request written to a
  busy board, a typed-ahead key — made the interrupt unreachable until reset,
  including against a saved boot routine stuck in `forever`. The poll now
  drains the console driver directly; measured interrupt latency behind
  buffered input is ~50 ms on hardware.
- **Input typed ahead of `console.read-line` survives the poll.** Drained
  non-interrupt bytes are kept in a small typeahead ring and handed to the
  next console read in order; a Ctrl-C discards input queued behind it. VM
  throughput is unchanged (spin benchmark at parity on `esp32_devkit_v1`).

## [0.1.9] - 2026-07-22

### Added

- **Wi-Fi can be composed out of a build.** `net` joins `ble` as an offered
  capability: a composition that turns it off drops the radio stack (~600 KB
  of flash on a classic ESP32). Libraries can declare hardware needs with
  `requires = ["i2s"]`.
- **Builds report their measured size.** Every ESP-IDF build writes
  `build/<board>/size.json`: the app image against the flashed partition
  table's app partition, plus IRAM/DRAM (and shared-pool DIRAM) usage.
  Reporting never fails a build that otherwise succeeded.

### Changed

- **The app partition is 2 MiB on every board.** All boards share one
  partition table sized to real hardware (4 MB flash floor; larger chips
  keep their profiles). An all-features image now keeps roughly a third of
  the partition free instead of 7%.
- **Error 8 is named `invalid`, not `bad source`.** The code is shared by
  the parser and by library precondition failures, and the old phrase lied
  outside the parse path. Parse failures keep their specific diagnostics
  (message and caret), so nothing is lost where the old name was right.

## [0.1.8] - 2026-07-21

### Added

- **Console programs can read one human-entered line.** `console.read-line:`
  waits for a complete data line without turning it into Frothy source, while
  Ctrl-C still interrupts the active evaluation.
- **Duration and digit-grouped literals.** `every 2s`, `after 500ms`,
  `pulse.add: wave, 1, 400ns`: `s` multiplies into milliseconds; `ms`, `us`,
  and `ns` pass through so a literal can name the unit its API documents.
  Underscores group digits in any base: `1_000_000`,
  `0b1000_0000_0000_0000_0000_0000`. All of it is lexer-level sugar over the
  same int.

### Changed

- **Native names follow one charter** (see CONTRIBUTING.md): predicates end
  in `?`, counts are nouns, no abbreviations, one spelling per word.
  Renamed: `ms` → `wait` (the sleep gets a verb; `millis`/`micros` clock
  reads are unchanged), `pad.len` → `pad.length`, `tcp.bytes-ready?` →
  `tcp.available`, `adc.above` → `adc.above?`, and the test-only
  `frothy.fire-event` → `frothy.event-fire`. The old spellings are gone —
  no aliases.
- **A call's arguments end where the next call begins — for every operator.**
  Previously only `+` stopped before a following call, so
  `fib: n - 1 - fib: n - 2` silently parsed as `fib: (n - 1 - fib: n - 2)`
  and recursed forever. Now `-`, `*`, `/`, `%`, comparisons, `and`, and `or`
  all follow the same sentence. Parentheses opt an inner call back into an
  argument: `gpio.write: pin, (1 - gpio.read: pin)`. `see` re-renders old
  code with the needed parens.
- **Chained comparison is a parse error.** `1 < 2 < 3` no longer evaluates
  `true < 3`; the error suggests joining two comparisons with `and`.
- **`not` negates a whole comparison.** `not x = 1` now means `not (x = 1)`,
  matching Python, Ruby, Lua, and SQL. Inside arithmetic, `not` needs
  parentheses.
- **`is` inside a block declares a local: `x is 5`.** Position decides scope:
  at the top level `is` binds a slot as before; in a body it declares a local
  in the innermost block. `here x is 5` still parses and means exactly the
  same thing. `set` never declares — setting an undeclared name now says
  "declare it first with is" alongside the near-miss suggestion.
- **Cell rows are asked for with a colon call: `readings is cells: 3`.** The
  old `cells(3)` spelling was the only parenthesized call shape in the
  language; parentheses now always mean grouping. The old form fails with a
  pointer to the new spelling.

## [0.1.7] - 2026-07-21

### Added

- **Seeed Studio XIAO ESP32C6 is an official board.** Firmware builds for the
  C6's RISC-V target, uses its native USB Serial/JTAG console and documented
  board pins, and ships in the web-flasher release bundle.

### Fixed

- **ESP-IDF portability follows each chip's hardware.** Application UARTs now
  count only high-power controllers, GPIO wake uses the wake mechanism offered
  by the selected target, and CPU clocks stay in chip-specific configuration.

## [0.1.6] - 2026-07-21

### Added

- **Firmware projects can remove offered capabilities.** A `[capabilities]`
  table in `frothy.toml` now accepts `ble = false` and carries that choice
  through the generated profile header and ESP-IDF configuration.

### Fixed

- **Library requirements are checked against the composed firmware.** A build
  rejects a library whose required capability was disabled, while requirements
  on known always-on capabilities remain valid.
- **Homebrew release instructions use the archive's version directly.** The
  formula template no longer expects a redundant version substitution.

## [0.1.5] - 2026-07-20

### Added

- **Runtime errors now show the rejected value.** Type, range, native-argument,
  handle, and busy-resource failures retain their compact numeric code while
  also reporting the value that caused the failure and, where useful, the
  expected kind or native argument position.

### Changed

- **Unsaved volatile state is a notice, not a failed programming session.** A
  `save` blocked by a live handle or buffer identifies the affected slot,
  terminates with `ok`, and leaves the device ready for the next form. The CLI,
  browser serial client, and VS Code extension preserve that distinction.
- **Browser editor and serial-client packages now live in Frothy App.** Core
  retains the language, firmware, CLI, VS Code extension, and human serial
  contract without owning browser project state or browser package builds.

### Fixed

- **CLI source framing can no longer mistake code for device status.** Reserved
  text such as `ok`, multiline forms, and status-looking source are sent in a
  source envelope; real device errors make `frothy send` exit nonzero and stop
  later file forms, including in records mode.
- **Interrupts settle once without losing the next form.** Host- and
  device-originated interrupts now complete the active response and preserve
  plain and records-mode file sequencing.
- **Firmware manifests include a checksum for every flash segment.** Consumers
  can validate each downloaded bootloader, partition-table, and application
  image before flashing.

## [0.1.4] - 2026-07-17

### Added

- **A bounded, inspectable Bluetooth Low Energy system.** Frothy can scan,
  advertise, hold one central or peripheral connection, install a small GATT
  server, and use one foreground GATT client procedure with short values,
  notifications, and indications.

### Changed

- **BLE state and limits stay visible through ordinary Frothy words.** Scan,
  connection, queue, procedure, error, and memory state remain inspectable,
  while `ble.off`, `clear`, and full recovery invalidate handles and release
  the radio.
- **The VS Code release artifact advanced to 0.5.1.** Its device vocabulary is
  fetched only when requested instead of appearing after every run.

### Fixed

- **Full recovery still erases saved state when BLE cleanup reports an error.**
  Restart then releases any remaining platform-owned state.

## [0.1.3] - 2026-07-14

### Added

- **A physical safe-boot path and a movable live console.** For 600 ms after a
  normal reset, Ctrl-C or a tap of the active-low BOOT button skips the saved
  user project and its `boot` word without erasing either one. `console.uart:`,
  `console.default:`, and `console.info:` can move, restore, and inspect the
  live REPL while that safe window always begins on the board default console.

- **Seeed Studio XIAO ESP32S3 is an official board.** Board manifests now own
  the chip, pins, LED polarity, and default console for both XIAO and ESP32
  DevKit V1. CI and release bundles build and identify both boards.

- **Bounded edge capture and timed pulse output.** The new `trace.*` words can
  record digital transitions and the new `pulse.*` words can build and play a
  timed waveform. The shipped examples use them to inspect I2C traffic and
  drive a WS2812 frame.

- **Newlines can separate expressions inside blocks.** Multiline `[...]`
  forms no longer need a semicolon at the end of each line; semicolons remain
  available when several expressions share one line. The CLI, browser editor,
  and VS Code now preserve those newlines when they send a form, and multiline
  errors point to the physical line that failed.

- **The editors now follow complete language forms and live device state.**
  The browser editor 0.2.1 and VS Code extension 0.4.0 run multiline forms,
  browse the connected device's words, surface device diagnostics, and keep
  connection and run state visible.

### Changed

- **The packaged CLI can flash official release firmware.** Board discovery,
  reset, wipe, and flash use the same manifests as the firmware build, while
  source-build commands consistently find the Frothy checkout root.

- **Web-flasher releases are board-complete segmented bundles.** Each release
  carries the ESP-IDF bootloader, partition table, and application segments at
  their generated flash addresses instead of one board-specific merged image.

### Fixed

- **Interrupting a running program returns the editor to idle immediately.**
  The friendly `ok — interrupted` line did not end with the wire protocol's
  required bare `ok`, so browser and CLI requests remained pending until a
  second Ctrl-C. Interrupts now report `interrupted`, terminate with `ok`, and
  return the normal prompt in one response.

- **ESP32 board behavior now follows board data.** ADC GPIO mapping, active-low
  LEDs, USB Serial/JTAG versus UART console selection, and schedulable
  millisecond waits no longer assume the DevKit V1 wiring everywhere.

## [0.1.2] - 2026-07-10

### Changed

- **Interrupting a running program is no longer reported as an error.**
  Stopping a loop with Ctrl-C or the boot button showed
  `error: interrupted (10)`; it now prints a calm `ok — interrupted`. The
  change is device-side, so the web editor and VS Code stop flagging a
  deliberate stop as a failure too.

- **A `save` that can't persist a value now says which slot and why.**
  Instead of a bare `unsupported (9)`, the error names the slot and the
  reason — for example `cannot save slot 'x' - bound to a word this firmware
  does not provide`.

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
