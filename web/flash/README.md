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

## esptool-js

`vendor/esptool-js/0.6.0/bundle.js` is the unmodified `package/bundle.js`
file from the v0.6.0 release at
https://github.com/espressif/esptool-js/releases/tag/v0.6.0 (217,616
bytes). The page imports it as an ES module — no CDN fetch at flash
time. To update, drop the new bundle in a sibling
`vendor/esptool-js/<version>/` directory, bump the import in `app.js`,
and update this note.
