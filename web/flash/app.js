// Page module: detect → (picker → flash → REPL) | fallback.
// Slice A wires only the detect + D6 fallback. Picker/flash/REPL land later.

const app = document.getElementById("app");
const fallback = document.getElementById("fallback");

// D6: WebSerial only, desktop only. Chrome Android exposes navigator.serial
// but only over Bluetooth RFCOMM, which cannot drive a USB-CDC ESP32.
const supported =
  "serial" in navigator && navigator.userAgentData?.mobile !== true;

if (!supported) {
  fallback.hidden = false;
} else {
  app.hidden = false;
}
