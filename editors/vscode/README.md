# Frothy for VS Code

Live Frothy workflow inside VS Code. The extension connects through the
`frothy session` CLI, runs complete language forms and files, shows compile
diagnostics, and inspects the connected device's live words.

## Install

The extension is not published in the Marketplace yet. Build and install the
current VSIX directly:

```sh
make vsix
code --install-extension editors/vscode/frothy-0.4.0.vsix
```

The extension expects `frothy` on `PATH`, or set `frothy.binaryPath`.

## Quick start

1. Open a `.fr` or `.frothy` file.
2. Click the **Frothy** status item to connect and choose a serial port if asked.
3. Put the cursor inside a complete form and press <kbd>Cmd/Ctrl</kbd>+<kbd>Enter</kbd> to **Run Form**.
4. Press <kbd>Cmd/Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Enter</kbd> to **Run File**.
5. Use **Frothy: Rerun** to repeat the last submitted form.

## Keyboard shortcuts

| Shortcut | Command | When |
|---|---|---|
| <kbd>Cmd/Ctrl</kbd>+<kbd>Enter</kbd> | Run Form | Connected and idle |
| <kbd>Cmd/Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Enter</kbd> | Run File | Connected and idle |
| <kbd>Cmd/Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>R</kbd> | Rerun | A previous form exists |
| <kbd>Cmd/Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>.</kbd> | Interrupt | Device is running |

## Editor surfaces

- **Status bar.** Shows disconnected, connecting, connected, running, stale,
  and error states. Click it to connect or disconnect.
- **Editor title.** Shows Run Form while idle and Interrupt while running.
- **Editor context menu.** Run Form, Run File, Rerun, and Inspect Word appear
  when valid for the current Frothy file and session state.
- **Problems.** Device-reported failures mark the complete form that failed.
- **Output channel.** Shows the serial session transcript while keeping focus
  in the source editor.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `frothy.binaryPath` | `"frothy"` | CLI path, absolute or on `PATH`. |
| `frothy.port` | `""` | Serial port; empty uses CLI discovery. |
| `frothy.baud` | `115200` | Serial baud. |
| `frothy.autoConnect` | `false` | Connect when a Frothy file opens. |

## Commands

All commands are available from the command palette under **Frothy: …**:

- **Connect / Disconnect** — manage the structured device session.
- **Run Form** — run one selected complete form or the form around the cursor.
- **Run File** — run every complete form in the current unsaved buffer.
- **Rerun** — repeat the last submitted form.
- **Open Example** — open a bundled example as an untitled Frothy file.
- **Browse Words** — search the connected device's live vocabulary.
- **Inspect Word** — run `see <word>` for the symbol under the cursor.
- **Show Status** — run `status`.
- **Memory** — run `mem`.
- **Save Overlay / Restore Overlay** — run `save` / `restore` on the device.
- **Interrupt** — send the device interrupt byte while code is running.
- **Show Output** — focus the Frothy output channel.
