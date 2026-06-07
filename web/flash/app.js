// Page module. Two REPL entry points:
//   1. Flash → auto-reveal "Connect REPL" → enter REPL
//   2. "Connect to board" on the picker → enter REPL on already-flashed device
// Both routes share enterRepl() so the REPL surface is identical.

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
  initRepl();
}

// Module-scope so the post-flash "Connect REPL" can reuse the same port
// without a second OS port dialog. Also drives the re-flash cleanup path.
let currentPort = null;
let reader = null;
let writer = null;
let readDone = null;
const encoder = new TextEncoder();

async function initPicker() {
  const manifest = await fetch("./firmware/manifest.json").then((r) => r.json());
  const boardSel = document.getElementById("board");
  const profileSel = document.getElementById("profile");
  const flashBtn = document.getElementById("flash");
  const connectExistingBtn = document.getElementById("connect-existing");

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
    if (row) flash(row, [boardSel, profileSel, flashBtn, connectExistingBtn]);
  });

  // "Connect to board" — REPL entry for an already-flashed board, bypasses
  // the flash step entirely so Connect REPL doesn't depend on re-flashing.
  connectExistingBtn.addEventListener("click", async () => {
    const status = document.getElementById("status");
    status.hidden = false;
    try {
      // requestPort needs transient activation; this is the click's first await.
      setStatus(status, "Pick a serial port…");
      await releaseCurrentPort();
      currentPort = await navigator.serial.requestPort();
      await enterRepl(/* fromFlash */ false);
    } catch (err) {
      setStatus(status, `Connect failed: ${err.message ?? err}`, true);
      currentPort = null;
    }
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
    await releaseCurrentPort();
    currentPort = await navigator.serial.requestPort();

    setStatus(status, "Fetching firmware…");
    const res = await fetch(`./${row.file}`);
    if (!res.ok) throw new Error(`firmware fetch ${res.status} for ${row.file}`);
    // D9: writeFlash data is Uint8Array in v0.6.0 (breaking change from v0.5.x).
    const data = new Uint8Array(await res.arrayBuffer());
    transport = new Transport(currentPort, false);

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

    // D13: hard-reset into Frothy. Transport.disconnect releases the port
    // so we can reopen it at 115200 in enterRepl. After disconnect the
    // device is mid-boot; entering the REPL immediately catches its banner.
    await loader.after("hard_reset");
    await transport.disconnect();
    transport = null;
    setStatus(status, "Flashed. Click Connect REPL to talk to the device.");
    repl.hidden = false;
  } catch (err) {
    setStatus(status, `Flash failed: ${err.message ?? err}`, true);
    if (transport) {
      try { await transport.disconnect(); } catch {}
    }
    await releaseCurrentPort();
    for (const el of lockables) el.disabled = false;
  }
}

// Cleanly close the cached SerialPort + any reader/writer locks. Safe to
// call when nothing is open. Bug B: without this between attempts, a
// second Flash click finds the port half-locked and esptool can't open it.
async function releaseCurrentPort() {
  if (reader) {
    try { await reader.cancel(); } catch {}
    try { reader.releaseLock(); } catch {}
    reader = null;
  }
  if (writer) {
    try { writer.releaseLock(); } catch {}
    writer = null;
  }
  if (readDone) {
    try { await readDone; } catch {}
    readDone = null;
  }
  if (currentPort) {
    try { await currentPort.close(); } catch {}
    currentPort = null;
  }
}

function initRepl() {
  const connectBtn = document.getElementById("connect");

  connectBtn.addEventListener("click", () => {
    enterRepl(/* fromFlash */ true);
  });

  document.getElementById("disconnect").addEventListener("click", leaveRepl);
}

async function enterRepl(fromFlash) {
  const status = document.getElementById("status");
  const repl = document.getElementById("repl");
  const connectBtn = document.getElementById("connect");
  const disconnectBtn = document.getElementById("disconnect");
  const log = document.getElementById("log");
  const line = document.getElementById("line");
  const picker = document.getElementById("picker");

  if (!currentPort) {
    setStatus(status, "No serial port held; pick one via Flash or Connect to board.", true);
    return;
  }

  try {
    await currentPort.open({ baudRate: 115200 });
  } catch (err) {
    setStatus(status, `REPL open failed: ${err.message ?? err}`, true);
    return;
  }

  writer = currentPort.writable.getWriter();
  log.textContent = "";
  readDone = readLoop(log);

  // UI: hide picker + Flash flow, show REPL controls
  picker.hidden = true;
  repl.hidden = false;
  connectBtn.hidden = true;
  log.hidden = false;
  line.hidden = false;
  disconnectBtn.hidden = false;
  setStatus(status, fromFlash
    ? "REPL connected at 115200 (post-flash)."
    : "REPL connected at 115200.");
  line.focus();

  // Wake nudge: if the device already emitted its boot banner before the
  // port reopen, send a newline to elicit an `ok` so the user sees the
  // REPL is alive instead of staring at an empty log.
  appendLog(log, "(connected — type a Frothy line and press Enter)\n");
  try {
    await writer.write(encoder.encode("\n"));
  } catch (err) {
    appendLog(log, `(wake newline failed: ${err.message ?? err})\n`);
  }

  // Bind keydown here (not in initRepl) so the writer reference is fresh
  // each REPL session. Remove on disconnect via the lineHandler reference.
  line.addEventListener("keydown", lineKeydown);
}

async function lineKeydown(e) {
  if (e.key !== "Enter" || !writer) return;
  e.preventDefault();
  const value = e.target.value;
  e.target.value = "";
  const log = document.getElementById("log");
  // Write-echo so the user sees their input went out even if the device
  // is slow or silent. Prefixed with > to distinguish from device output.
  appendLog(log, `> ${value}\n`);
  try {
    await writer.write(encoder.encode(value + "\n"));
  } catch (err) {
    appendLog(log, `(write failed: ${err.message ?? err})\n`);
  }
}

async function leaveRepl() {
  const log = document.getElementById("log");
  const line = document.getElementById("line");
  const repl = document.getElementById("repl");
  const picker = document.getElementById("picker");
  const status = document.getElementById("status");
  const connectBtn = document.getElementById("connect");
  const disconnectBtn = document.getElementById("disconnect");

  line.removeEventListener("keydown", lineKeydown);
  await releaseCurrentPort();
  log.textContent = "";
  line.value = "";
  log.hidden = true;
  line.hidden = true;
  disconnectBtn.hidden = true;
  connectBtn.hidden = false;
  repl.hidden = true;
  picker.hidden = false;
  // Re-enable any picker controls that may still be disabled from a prior
  // Flash attempt (the success-path leaves them disabled until disconnect).
  for (const id of ["flash", "board", "profile", "connect-existing"]) {
    document.getElementById(id).disabled = false;
  }
  setStatus(status, "Disconnected.");
}

async function readLoop(log) {
  reader = currentPort.readable.getReader();
  const decoder = new TextDecoder("utf-8");
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      appendLog(log, decoder.decode(value, { stream: true }));
    }
  } catch (err) {
    appendLog(log, `(read loop ended: ${err.message ?? err})\n`);
  } finally {
    try { reader.releaseLock(); } catch {}
  }
}

function appendLog(log, text) {
  log.textContent += text;
  log.scrollTop = log.scrollHeight;
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
