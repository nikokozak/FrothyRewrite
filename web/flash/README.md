# Frothy web flasher

A static page that flashes Frothy firmware to an ESP32 board over
WebSerial, then drops you into a minimal in-browser REPL.

## Serve locally

```sh
python3 -m http.server 8000
```

Open `http://127.0.0.1:8000/web/flash/` in Chrome, Edge, Opera, or
Firefox 151+ on desktop. `file://` does not work — ES modules and
WebSerial both need a real HTTP origin.

## Hosted

GitHub Pages serves the same files at
`https://nikokozak.github.io/FrothyRewrite/web/flash/`.

## Status

Slice A landed: detect + D6 fallback. Picker, flash, and REPL ship in
later slices.
