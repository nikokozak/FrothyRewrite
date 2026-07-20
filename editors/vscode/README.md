# Frothy for VS Code

Live Frothy workflow inside VS Code. The extension connects through the
`frothy` CLI, runs complete language forms and files, shows compile
diagnostics, inspects the connected device's live words, and drives the
firmware lifecycle (flash, build, install, recover) from a dedicated sidebar.

## Install

The extension is not published in the Marketplace yet. Build and install the
current VSIX directly:

```sh
make vsix
code --install-extension editors/vscode/frothy-0.5.2.vsix
```

The extension expects `frothy` on `PATH`, or set `frothy.binaryPath`.

## Quick start

1. Open a `.fr` or `.frothy` file — or the **Frothy** sidebar (cup icon).
2. Follow **Getting Started** in the Project view, or click **Connect** in the
   Device view and choose a serial port if asked.
3. Put the cursor inside a complete form and press
   <kbd>Cmd/Ctrl</kbd>+<kbd>Enter</kbd>, or click the **▷ Run** lens above it.
4. Press <kbd>Cmd/Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Enter</kbd> to **Run File**.

Not connected yet? Any run action offers **Connect & Run**.

## Keyboard shortcuts

| Shortcut | Command |
|---|---|
| <kbd>Cmd/Ctrl</kbd>+<kbd>Enter</kbd> | Run Form |
| <kbd>Cmd/Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Enter</kbd> | Run File |
| <kbd>Cmd/Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>R</kbd> | Rerun |
| <kbd>Cmd/Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>.</kbd> | Interrupt (while running) |

## Editor surfaces

- **Frothy sidebar.** The Activity Bar cup icon opens three views:
  - **Device** — connection state, port, board profile and mode, plus
    one-click Status, Memory, Save Overlay, and Restore Overlay.
  - **Words** — the device's vocabulary, fetched when you hit the view's
    refresh button. Click a word to inspect it (`see <word>`).
  - **Project** — the firmware lifecycle: Getting Started, New Project,
    Build, Flash, Install Library, Open Example, REPL, Doctor, CLI Menu,
    Stop Serial Sessions, Wipe User Definitions.
- **▷ Run lenses.** Every complete top-level form gets a run lens
  (`frothy.codeLens` turns them off).
- **Live completion.** Autocomplete offers the device's words from the last
  Words refresh.
- **Status bar.** Shows disconnected, connecting, connected, running, stale,
  and error states. Click it to connect, disconnect, or interrupt.
- **Editor title / context menu.** Run Form, Run File, Rerun, and Inspect
  Word for the current Frothy file.
- **Problems.** Device-reported failures mark the complete form that failed.
- **Output channel.** The serial session transcript, colored, keeping focus
  in the source editor.
- **Frothy terminal.** Lifecycle verbs (flash, build, doctor, REPL, …) run in
  a shared `Frothy` terminal so their output and prompts stay interactive.
  Verbs that need the serial port disconnect the session first; click the
  status bar item to reconnect afterwards.
- **Getting Started walkthrough.** Install CLI → flash → connect → run →
  explore, from VS Code's Welcome page or the Project view.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `frothy.binaryPath` | `"frothy"` | CLI path, absolute or on `PATH`. |
| `frothy.port` | `""` | Serial port; empty uses CLI discovery. |
| `frothy.baud` | `115200` | Serial baud. |
| `frothy.autoConnect` | `false` | Connect when a Frothy file opens. |
| `frothy.codeLens` | `true` | ▷ Run lens above each complete form. |

## Commands

Session — **Connect / Disconnect**, **Run Form**, **Run File**, **Rerun**,
**Interrupt**, **Browse Words**, **Inspect Word**, **Show Status**,
**Memory**, **Save Overlay / Restore Overlay**, **Show Output**,
**Open Example**, **Refresh Words**.

Lifecycle (run in the Frothy terminal via the CLI) — **Flash Firmware**,
**Build Firmware**, **Install Project Library**, **New Project**, **Doctor**,
**Open REPL in Terminal**, **Open CLI Menu**, **Stop Serial Sessions**,
**Wipe User Definitions**.
