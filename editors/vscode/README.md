# Frothy for VS Code

A thin VS Code extension over the `frothy` CLI. One `frothy connect` child per
workspace; commands write to its stdin; device output renders in an output
channel named **Frothy**.

## Install

Sideload only. Build the VSIX from the repo root, then install it:

```sh
make vsix
code --install-extension editors/vscode/frothy-0.1.0.vsix
```

The extension expects `frothy` on `PATH`, or set `frothy.binaryPath` to an
absolute path.

## Settings

| Setting              | Default     | Meaning                                     |
| -------------------- | ----------- | ------------------------------------------- |
| `frothy.binaryPath`  | `"frothy"`  | Path to the CLI. Absolute or on `PATH`.     |
| `frothy.port`        | `""`        | Serial port. Empty = CLI port discovery.    |
| `frothy.baud`        | `115200`    | Serial baud.                                |
