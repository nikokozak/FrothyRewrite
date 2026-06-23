# Frothy

<!-- PROSE: one-paragraph tagline. What Frothy is, who it's for, the
     single sentence that makes a stranger want to keep reading. -->

## Try Frothy in 60 seconds (no install)

Have an ESP32 DevKit V1 plugged in over USB and a recent Chrome or Edge
on a desktop? You can be running Frothy without installing anything.

1. **Flash** the firmware: [frothy flasher](https://nikokozak.github.io/FrothyRewrite/web/flash/).
2. **Edit and run** code: [frothy editor](https://nikokozak.github.io/FrothyRewrite/web/editor/).

Both pages talk to the board over WebSerial. No Frothy install on your
machine, no toolchain to set up.

<!-- PROSE: one or two sentences framing what the reader will see —
     "you'll be defining words and watching the chip respond in
     seconds" — set the right expectation. -->

## Develop on your machine

For local development, anything you can do on the hosted pages plus
deeper inspection, file editing, and CI:

```sh
# 1. Get the source.
git clone https://github.com/nikokozak/FrothyRewrite
cd FrothyRewrite

# 2. Build the host CLI and put it on your PATH.
#    `make cli` prints the exact export line for your machine; add it to
#    ~/.zshrc (or ~/.bashrc) and restart the terminal to make it stick.
make cli

# 3. Install ESP-IDF (one-time, ~20 min, no sudo).
frothy bootstrap

# 4. Check the host setup.
frothy doctor

# 5. Flash a board (builds the firmware from source, then writes it over serial).
frothy flash esp32_devkit_v1 --port /dev/cu.usbserial-0001

# 6. Talk to the board.
frothy connect --port /dev/cu.usbserial-0001
```

Every verb prints `--help` with description, examples, and flags.

<!-- PROSE: one sentence on what the reader should do if they get
     stuck on any step (link to issues? frothy doctor?). -->

## Write your first Frothy program

```sh
frothy init my-sketch
cd my-sketch
# Edit main.fr in your favorite editor.
frothy send main.fr --port /dev/cu.usbserial-0001
```

Or stay interactive — `frothy connect` opens a REPL where you type
Frothy lines directly at the board.

<!-- PROSE: a 4-6 line code sample showing what an idiomatic Frothy
     program looks like. The web editor's default sketch is:
       to greet [ "hello, world" ]
       greet
     Use that, or something more compelling. -->

## Edit Frothy code

| Where you edit | How |
|---|---|
| **Any text editor** | Write a `.fr` file. Send it with `frothy send file.fr --port …` or watch it on the device with `frothy connect`. |
| **VS Code** | Install the extension shipped at `editors/vscode/frothy-0.1.0.vsix`. Adds Frothy syntax + a "Connect" command. |
| **In the browser** | Open the [frothy editor](https://nikokozak.github.io/FrothyRewrite/web/editor/). WebSerial talks to the chip directly; sketches save to localStorage. |

## How Frothy is put together

Frothy's runtime stays small enough to read end to end:

- `src/`: core runtime, parser, compiler, VM, image, persistence, slots, tagged values.
- `profiles/`: feature and word-size choices per target class.
- `boards/`: board definitions (`esp32_devkit_v1`, `host`, `arduino_uno`).
- `targets/`: host, AVR, and ESP-IDF platform glue.
- `cmd/frothy-session/`: the `frothy` CLI (Go).
- `libs/frothy-repl/`, `libs/frothy-editor/`: ESM libraries that wrap
  the wire protocol for browser and Node consumers.
- `web/flash/`, `web/editor/`: static demo pages that vendor the
  libraries above.
- `test/`: one C test binary.

The first interface is a human serial session — `cat`, `screen`,
`tio`, or any terminal can drive a Frothy board directly. The CLI and
the web tools are conveniences, not a private control plane.

## Tests and checks

```sh
make test                                # core C test binary
make test-tiny-328                       # tiny-AVR profile
make test-host-normal                    # roomy host profile
make test-esp32-plain-host-transcript    # ESP32 transcript replay
go test ./cmd/frothy-session/...         # the Go CLI
(cd libs/frothy-repl && npm test)        # @frothy/repl
(cd libs/frothy-editor && npm test)      # @frothy/editor
```

## Status

Frothy is in active rewrite. Module 0.1 (talk to your board) and 0.2
(tier-3, editor surface, network) have shipped. Module 0.3 (editor
pivot + hardening) is in progress; the web editor, web flasher, CLI
bootstrap, and curated `--help` story landed in this round.

<!-- PROSE: one or two sentences pitching what Frothy is *for* in a
     pedagogical / hardware-teaching context. This is what a grant
     reader (or a teacher considering Frothy for a classroom) will
     latch onto. -->

## License

<!-- PROSE: confirm the project license. Existing tree carries no
     LICENSE file at root; pick one before public attention. -->
