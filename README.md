# Frothy

[![CI](https://github.com/nikokozak/FrothyRewrite/actions/workflows/ci.yml/badge.svg)](https://github.com/nikokozak/FrothyRewrite/actions/workflows/ci.yml)

Frothy is a small language for live-coding microcontrollers over a serial
line. You define words and send them to the board; it evaluates them and
answers back — no compile-flash-reboot loop, no toolchain to install, no
filesystem to mount. It is built for people learning to program hardware and
for anyone who wants a chip that talks back: the runtime is small enough to
read end to end, and the language is small enough to keep in your head.

## Try Frothy in 60 seconds (no install)

Have an ESP32 DevKit V1 plugged in over USB and a recent Chrome or Edge
on a desktop? You can be running Frothy without installing anything.

1. **Flash** the firmware: [frothy flasher](https://frothy.dev/flash/).
2. **Edit and run** code: [frothy editor](https://frothy.dev/editor/).

Both pages talk to the board over WebSerial. No Frothy install on your
machine, no toolchain to set up.

Within a few seconds you'll be defining words and watching the board act on
them — set a pin, blink the onboard LED, print a value — one line at a time,
the way you'd talk to a REPL.

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

Stuck on any step? Run `frothy doctor` — it checks your setup and names the
fix for each problem it finds. Most first-run snags (no board attached, wrong
port, ESP-IDF not installed) show up there before anything else does.

## Write your first Frothy program

```sh
mkdir my-sketch && cd my-sketch && frothy init
# Edit main.fr in your favorite editor.
frothy send main.fr --port /dev/cu.usbserial-0001
```

Or stay interactive — `frothy connect` opens a REPL where you type
Frothy lines directly at the board.

A first Frothy program defines a word and runs it. The board answers on the
next line:

```
-- Define a word with `to`, then call it with a trailing colon.
to greet [ "hello from your board" ]
greet:
-- greet: answers  "hello from your board"

-- Blink the onboard LED five times, 200 ms on and 200 ms off.
blink: $led_builtin, 5, 200
```

Inside `[...]`, put each expression on its own line and no ending punctuation
is needed. Use `;` only when you want several expressions on the same line.

Words you define stick around on the board, so you build a program by
teaching the chip one word at a time.

### Move the live console to another UART

Frothy's REPL can run over any suitable 3.3 V UART pair, not only the board's
USB or boot-serial connection. This moves the next reply and all later input
to TX 25, RX 34 at 1200 baud:

```frothy
console.uart: 25, 34, 1200
```

To make that route part of a project, put it in `boot` and save normally:

```frothy
to boot [
  console.uart: 25, 34, 1200
]
save
```

On every reset the board first starts on its fixed default console and opens a
600 ms safe-boot window. Send Ctrl-C or, after resetting normally, tap BOOT
during that window to skip the saved project and keep the default console. Do
not hold BOOT through reset: GPIO0 also selects the ESP32 ROM bootloader.
`console.default:` returns a live session to the board default, and
`console.info:` prints the active route.

`clear` does not disrupt the live connection. If you then `save` and reboot,
the cleared project no longer contains the saved `boot` reroute, so the board
stays on its default console.

ESP32 GPIO is 3.3 V logic. A device with true RS-232 voltage levels needs a
level converter such as a MAX3232; do not connect RS-232 lines directly to the
board.

## Edit Frothy code

| Where you edit | How |
|---|---|
| **Any text editor** | Write a `.fr` file. Send it with `frothy send file.fr --port …` or watch it on the device with `frothy connect`. |
| **VS Code** | Build the 0.4.0 extension with `make vsix`, then install `editors/vscode/frothy-0.4.0.vsix`. Adds syntax, Run Form/File, live words, diagnostics, and serial session controls. |
| **In the browser** | Open the [frothy editor](https://frothy.dev/editor/). WebSerial talks to the chip directly; sketches autosave locally. |

## How Frothy is put together

Frothy's runtime stays small enough to read end to end:

- `src/`: core runtime, parser, compiler, VM, image, persistence, slots, tagged values.
- `profiles/`: feature and capacity choices per target class.
- `boards/`: board definitions (`esp32_devkit_v1`, `host`).
- `targets/`: host and ESP-IDF platform glue.
- `cmd/frothy-session/`: the `frothy` CLI (Go).
- `libs/frothy-repl/`, `libs/frothy-editor/`: ESM libraries that wrap
  the wire protocol for browser and Node consumers.
- `tools/build-editor-bundle.sh`, `tools/build-flasher-bundle.sh`: build the
  browser artifacts vendored into FrothySite.
- `test/`: core C tests, Unity tests, transcript replays, and library e2e fixtures.

The first interface is a human serial session — `cat`, `screen`,
`tio`, or any terminal can drive a Frothy board directly. The CLI and
the web tools are conveniences, not a private control plane.

## Tests and checks

Run `make help` to see the common make targets.
The check surface is the core C suite, 10 Unity binaries, transcript replays,
the library e2e fixture, Go packages, and the TypeScript library suites.

```sh
make test                                # core C suite
make test-unity                          # Unity host binaries
make test-host-normal                    # roomy host profile
make test-esp32-plain-host-transcript    # ESP32 transcript replay
make test-lib-e2e                        # library extension e2e fixture
go test ./cmd/... ./internal/...         # Go packages
(cd libs/frothy-repl && npm test)        # @frothy/repl
(cd libs/frothy-editor && npm test)      # @frothy/editor
```

## Status

Frothy is in active rewrite. Module 0.1 (talk to your board) and 0.2
(tier-3, editor surface, network) have shipped. Module 0.3 (editor
pivot + hardening) is in progress; the web editor, web flasher, CLI
bootstrap, and curated `--help` story landed in this round.

Frothy is built for teaching the feel of hardware. The loop from idea to a
chip doing something is seconds long, the whole language fits on a page, and
nothing you write can leak memory or fragment its way to a mysterious crash —
Frothy programs never allocate at all. It aims to be the shortest path from
"I have a board" to "I made it do something," whether that's in a classroom, a
workshop, or your own first afternoon with a microcontroller.

## License

See [CONTRIBUTING.md](CONTRIBUTING.md) for setup, checks, and PR guidance.

Frothy is released under the [MIT License](LICENSE).
