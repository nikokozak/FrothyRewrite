# Frothy for VS Code

Live Frothy workflow inside VS Code. Connects to a Frothy-flashed board
via the `frothy session` CLI subprocess, lets you run lines, selections,
and whole buffers at the chip, and shows device output in an output
channel named **Frothy**.

## Install

```sh
make vsix
code --install-extension editors/vscode/frothy-0.2.1.vsix
```

The extension expects `frothy` on `PATH`, or set `frothy.binaryPath`.

## Quick start

1. Open a `.fr` or `.frothy` file.
2. Click the **Frothy** indicator in the status bar (bottom-left) to connect.
3. Put your cursor on a line and hit <kbd>Shift</kbd>+<kbd>Enter</kbd> to run it.
4. Select multiple lines and hit <kbd>⌘</kbd>+<kbd>Enter</kbd> (<kbd>Ctrl</kbd>+<kbd>Enter</kbd> on Linux/Windows) to run them in order.
5. Hit <kbd>⌘</kbd>+<kbd>⌥</kbd>+<kbd>R</kbd> to repeat the last form.

## Keyboard shortcuts

| Shortcut | Command | When |
|---|---|---|
| <kbd>Shift</kbd>+<kbd>Enter</kbd> | Run line | Connected |
| <kbd>⌘</kbd>+<kbd>Enter</kbd> | Run selection (or current line) | Connected |
| <kbd>⌘</kbd>+<kbd>⌥</kbd>+<kbd>R</kbd> | Repeat last form | Have a last form |
| <kbd>⌘</kbd>+<kbd>Shift</kbd>+<kbd>Enter</kbd> | Send current buffer through the open session | Connected |
| <kbd>⌘</kbd>+<kbd>⌥</kbd>+<kbd>B</kbd> | See word under cursor | Connected |
| <kbd>⌘</kbd>+<kbd>⌥</kbd>+<kbd>.</kbd> | Interrupt device | Connected |

On Linux and Windows, swap <kbd>⌘</kbd> for <kbd>Ctrl</kbd>.

## Editor surfaces

- **Status bar (left).** Shows `$(circle-slash) Frothy` when disconnected,
  `$(plug) Frothy: <port>` when connected. Click to toggle.
- **Editor title bar.** Inline icons for Connect / Run line / Run selection /
  Rerun last / Send file / Disconnect — visible whenever a Frothy file is
  focused.
- **Editor context menu.** Run line, Run selection, See word — when right-clicking
  inside a Frothy file with a connection open.
- **Output channel "Frothy".** Reads as a transcript — prompts, source
  echoes, and replies come from the running session. Errors are
  red, `ok` is green, status lines are italic-grey, sent lines are bold
  blue, `mem` key/value pairs are paired. Auto-reveals when you run a
  command (cursor stays in the editor).

## Settings

| Setting               | Default     | Meaning                                          |
|-----------------------|-------------|--------------------------------------------------|
| `frothy.binaryPath`   | `"frothy"`  | Path to the CLI. Absolute or on `PATH`.          |
| `frothy.port`         | `""`        | Serial port. Empty = CLI auto-discovery.         |
| `frothy.baud`         | `115200`    | Serial baud.                                     |
| `frothy.autoConnect`  | `false`     | Auto-connect when opening a Frothy file.         |

## Commands (full list)

All accessible via the command palette under **Frothy: …**:

- **Connect / Disconnect** — manage the `frothy session` subprocess.
- **Run Line** — send the current line.
- **Send Selection** — send every non-empty selected line, in order.
- **Run Last Form** — repeat the last line / form sent.
- **Send File** — send the current editor buffer through the open session as a `.source` block. Unsaved changes are included; saved file paths are used only to resolve `include "..."`.
- **See Word** — `see <word>` for the symbol under the cursor.
- **List Words** — `words`.
- **Show Status** — `status`.
- **Memory** — `mem`.
- **Save Overlay** / **Restore Overlay** — `save` / `restore`.
- **Interrupt** — sends device interrupt byte (0x03) to the running session.
- **Show Output** — focus the Frothy output channel.
