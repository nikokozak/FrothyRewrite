# Frothy Rewrite

Frothy is a small C runtime and language for live device work over serial.

The model is meant to stay plain:

- source text is parsed and compiled into instruction bytes
- slots hold tagged words
- the VM runs code objects
- base image rows declare built-in names, literals, and native calls
- profiles choose what fits on a target
- boards provide the few platform calls the runtime needs

The first interface is a human serial session. Host tools can help compile,
mirror, and test, but they are helpers, not a private way around the device.

Current tree:

- `src/`: core runtime, parser, compiler, VM, image, persistence, slots, tagged
  values
- `profiles/`: feature and word-size choices
- `boards/`: board definitions
- `targets/`: host, AVR, and ESP-IDF platform glue
- `cmd/frothy-session/`: serial helper
- `test/`: one C test binary

Common checks:

```sh
make test
make test-tiny-328
make test-host-normal
make test-esp32-plain-host-transcript
```

This is still a rewrite. The job is to keep the useful live-device behavior
small enough to read, test, recover, and port.
