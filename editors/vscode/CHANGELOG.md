# Changelog

## 0.4.0 — 2026-07-10

The editor now follows the structured Frothy session contract and runs whole
language forms.

### Added

- Native connecting, running, stale, and error states derived from
  `frothy session --records`.
- **Run Form**, **Run File**, and **Rerun** commands. Run Form accepts one
  selected form or finds the form around the cursor.
- Full-form diagnostics for host compile errors without pretending the device
  reported an exact character offset.
- **Browse Words** and **Inspect Word**, both backed by the connected device's
  live vocabulary.
- A native port picker when CLI discovery finds more than one serial device.
- Native recovery actions for a missing CLI or missing serial device.

### Changed

- The old Run Line, Send Selection, Run Last Form, and Send File command IDs
  were removed. This is a deliberate pre-release break rather than a layer of
  compatibility aliases.
- Cmd/Ctrl+Enter runs one form; Cmd/Ctrl+Shift+Enter runs the current file.
- The extension activates for Frothy files and commands, not every VS Code
  startup. Its status item stays hidden outside a Frothy editing/session
  context.

### Fixed

- A spawned CLI process no longer counts as connected. The extension waits for
  a valid device status record.
- Split or coalesced stdout chunks no longer corrupt session state.
- Multiline definitions are submitted and remembered as one form.

## 0.3.0 — 2026-07-09

Editor-pass release: an example browser and corrected language support.

### Added

- **Frothy: Open Example** — a QuickPick of the bundled example sketches. The
  chosen one opens as an untitled buffer, ready to Send File. The set is the
  same `examples/` the browser editor and the host smoke battery use.

### Fixed

- **Comment syntax was wrong.** Line comments were configured as `//`, which the
  parser rejects; they are now `--`, with `-* *-` block comments. `Cmd+/` now
  inserts a comment the device accepts.

### Changed

- **Grammar synced to the language.** The keyword set now matches the parser
  (adds `attempt`, `rescue`, `here`, `while`, `as`, `record`, and more),
  `--` / `-* *-` comments are recognized, and `nil` colors as a constant.

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
- **Batch headers** for multi-line work, so the per-line echo doesn't
  blur together at scale:
  - Multi-line **Send Selection** prepends
    `> [run selection: N lines]`.
  - **Send File** brackets the subprocess with
    `> [send file: foo.fr · 12 lines]` before, and
    `> [send file: foo.fr · ok]` (or `exit N` on failure) after.
  Single-line runs keep their bare `> <text>` echo — bookends only
  kick in when there's actually a batch to bracket.

### Fixed

- The output channel was effectively write-only and invisible by
  default. Pairing user input with device output in one panel closes
  that gap.
- `Send File` previously emitted no marker into the transcript — a
  50-line file produced a stream of device responses with no hint of
  which file caused them. Header/footer markers fix this.

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
