# Examples

`examples/` holds complete Frothy programs for reading, sending to a board, and
running as the host smoke battery. The same files are intended to feed the
future editor example picker.

Each file declares tags in its header:

- `@tag host` runs under `make examples`.
- `@tag device` needs a connected board.
- `@tag hardware` needs a physical peripheral.

Run the host-safe examples with:

```sh
make examples
```

Send one example to a board with:

```sh
frothy send examples/08-blink.fr
```
