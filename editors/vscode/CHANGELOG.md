# Changelog

## 0.2.1 — 2026-06-20

Polish pass on serial reply rendering.

### Added

- **Echo of what was sent.** Every line the extension writes to the
  device first appears in the Frothy output channel as `> <text>`,
  so the user sees their command paired with the device's reply.
  Reads as a real transcript instead of a stream of device-only bytes.
- **Frothy Transcript grammar** for the output channel. The channel
  now uses its own language ID (`frothy-transcript`) with a TextMate
  grammar that colors errors red, `ok` green, status lines italic
  grey, sent lines bold blue, and `mem` key/value pairs paired.
  Themes the user already has do all the styling — no theme overrides.
- **Auto-reveal on send.** The output channel becomes visible
  whenever the extension writes to the device. `preserveFocus: true`
  keeps the cursor in the editor.

### Fixed

- The output channel was effectively write-only and invisible by
  default. Pairing user input with device output in one panel closes
  that gap.

## 0.2.0 — 2026-06-20

A real polish pass. The extension was thin to the point of clunky; this
release adds the VS Code surfaces a Frothy workflow wants — without
giving up the small CLI-subprocess backbone.

### Added

- **Status bar item** on the left, with `$(plug)` when connected (showing
  the port) and `$(circle-slash)` when disconnected. Click to toggle.
- **Keyboard shortcuts** for the daily-use verbs:
  - <kbd>Shift</kbd>+<kbd>Enter</kbd> — Run line
  - <kbd>⌘</kbd>+<kbd>Enter</kbd> — Run selection
  - <kbd>⌘</kbd>+<kbd>⌥</kbd>+<kbd>R</kbd> — Repeat last form
  - <kbd>⌘</kbd>+<kbd>Shift</kbd>+<kbd>Enter</kbd> — Send file
  - <kbd>⌘</kbd>+<kbd>⌥</kbd>+<kbd>B</kbd> — See word
  - <kbd>⌘</kbd>+<kbd>⌥</kbd>+<kbd>.</kbd> — Interrupt
- **Editor title bar icons** for Connect / Run line / Run selection /
  Rerun last / Send file / Disconnect, visible whenever a Frothy file is
  focused.
- **Editor context menu** entries for Run line / Send selection / See word.
- **New commands**: `Run Line`, `Run Last Form`, `Disconnect`, `Show Status`,
  `Memory`, `Show Output`.
- **Auto-connect** setting (`frothy.autoConnect`) that connects when a
  Frothy file is opened.
- **Language configuration** (bracket matching, indent rules, line
  comments).
- **`.fr` file extension** alongside `.frothy`.
- **Grammar additions** — `is`, `fn`, `set`, `forever`, `on`, `every`,
  `boot`, `rising`, `falling`, `changes`; line comments (`//`).

### Changed

- `Send Selection` now runs the current line when there's no selection,
  matching the pattern from the old Frothy extension. The previous
  behavior (no-op without a selection) was a usability dead end.
- The "not connected" warning surfaces via `window.showWarningMessage`
  instead of writing to the output channel where it scrolled past.

### Fixed

- **Spam-clicking Connect or the status bar item** no longer spawns
  multiple concurrent `frothy connect` subprocesses. A re-entrancy
  guard returns the in-flight promise to repeat callers.
- **`autoConnect` race when several `.fr` files open at once** —
  same guard, no churn.
- **`writeLine` / `writeByte` no longer throw on EPIPE** when the
  child dies between `isConnected()` and the actual stdin write.
  The write returns `false` and callers (`runLine`, `sendSelection`,
  `runLast`) surface the disconnect warning.
- **`sendSelection` stops on first failed write** instead of dropping
  the rest of the selection silently. `lastForm` reflects the last
  line that actually went out.

## 0.1.0

First sideload release. Eight palette commands over a single
`frothy connect` child per workspace. Output channel rendering.
TextMate grammar for `.frothy` files.
