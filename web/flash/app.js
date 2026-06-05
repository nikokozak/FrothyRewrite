// Page module: detect → (picker → flash → REPL) | fallback.
// Slice A: detect + D6 fallback. Slice B: manifest picker + D8 persistence.
// Slice C: flash flow via vendored esptool-js (D4, D9).

import { ESPLoader, Transport } from "./vendor/esptool-js/0.6.0/bundle.js";

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
  initPicker();
}

async function initPicker() {
  const manifest = await fetch("./firmware/manifest.json").then((r) => r.json());
  const boardSel = document.getElementById("board");
  const profileSel = document.getElementById("profile");
  const flashBtn = document.getElementById("flash");

  const boards = [...new Set(manifest.map((row) => row.board))];
  for (const b of boards) boardSel.append(option(b));
  const savedBoard = localStorage.getItem("frothy.flash.board");
  if (boards.includes(savedBoard)) boardSel.value = savedBoard;

  function renderProfiles() {
    profileSel.replaceChildren();
    const profiles = manifest
      .filter((row) => row.board === boardSel.value)
      .map((row) => row.profile);
    for (const p of profiles) profileSel.append(option(p));
    const savedProfile = localStorage.getItem("frothy.flash.profile");
    if (profiles.includes(savedProfile)) profileSel.value = savedProfile;
  }
  renderProfiles();

  boardSel.addEventListener("change", () => {
    localStorage.setItem("frothy.flash.board", boardSel.value);
    renderProfiles();
  });
  profileSel.addEventListener("change", () => {
    localStorage.setItem("frothy.flash.profile", profileSel.value);
  });

  flashBtn.addEventListener("click", () => {
    const row = manifest.find(
      (r) => r.board === boardSel.value && r.profile === profileSel.value,
    );
    if (row) flash(row, [boardSel, profileSel, flashBtn]);
  });
}

async function flash(row, lockables) {
  const status = document.getElementById("status");
  const repl = document.getElementById("repl");
  status.hidden = false;
  for (const el of lockables) el.disabled = true;

  let transport = null;
  try {
    // requestPort needs transient activation. Call it before any other await
    // so the click's user gesture isn't consumed by a slow firmware fetch.
    setStatus(status, "Pick a serial port…");
    const port = await navigator.serial.requestPort();

    setStatus(status, "Fetching firmware…");
    const res = await fetch(`./${row.file}`);
    if (!res.ok) throw new Error(`firmware fetch ${res.status} for ${row.file}`);
    // D9: writeFlash data is Uint8Array in v0.6.0 (breaking change from v0.5.x).
    const data = new Uint8Array(await res.arrayBuffer());
    transport = new Transport(port, false);

    setStatus(status, "Connecting…");
    const loader = new ESPLoader({
      transport,
      baudrate: 921600,
      romBaudrate: 115200,
    });
    const chip = await loader.main();
    setStatus(status, `Connected to ${chip}. Flashing…`);

    await loader.writeFlash({
      fileArray: [{ address: 0x0, data }],
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: false,
      compress: true,
      reportProgress: (_i, written, total) => {
        const pct = total ? Math.round((written / total) * 100) : 0;
        setStatus(status, `Flashing… ${pct}%`);
      },
    });

    // D13: hard-reset into Frothy, then surface "Connect REPL".
    await loader.after("hard_reset");
    await transport.disconnect();
    setStatus(status, "Flashed. Connect the REPL when ready.");
    repl.hidden = false;
  } catch (err) {
    setStatus(status, `Flash failed: ${err.message ?? err}`, true);
    if (transport) {
      try { await transport.disconnect(); } catch {}
    }
    for (const el of lockables) el.disabled = false;
  }
}

function setStatus(el, text, isError = false) {
  el.textContent = text;
  el.classList.toggle("err", isError);
}

function option(value) {
  const opt = document.createElement("option");
  opt.value = value;
  opt.textContent = value;
  return opt;
}
