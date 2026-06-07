// Two REPL entry points (flash → connect-after, or connect-existing) share
// enterRepl(). Designed deliberately thin: no wake newlines, no write echo,
// no Frothy-shaped reformatting. The device echoes input and prints its own
// prompt; the page is a window onto that traffic.

import { ESPLoader, Transport } from "./vendor/esptool-js/0.6.0/bundle.js";

const app = document.getElementById("app");
const fallback = document.getElementById("fallback");

const supported =
  "serial" in navigator && navigator.userAgentData?.mobile !== true;

if (!supported) {
  fallback.hidden = false;
} else {
  app.hidden = false;
  initPicker();
  initRepl();
  // Page close / reload: release the port so a re-flash from a new tab works.
  // Without this Chrome/Arc holds the FD open at the OS level and the next
  // esptool open fails with "Failed to connect."
  window.addEventListener("beforeunload", () => {
    if (currentPort) {
      try { currentPort.close(); } catch {}
    }
  });
}

let currentPort = null;
let reader = null;
let writer = null;
let readDone = null;
const encoder = new TextEncoder();

// Bounded log. Each line is a text node; we trim from the head when the count
// exceeds the cap. Avoids the O(n²) textContent += pattern that locks the
// page when the device blasts hundreds of `ok\n> ` lines on boot replay.
const LOG_MAX_NODES = 800;

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

  connectExistingBtn.addEventListener("click", async () => {
    const status = document.getElementById("status");
    status.hidden = false;
    try {
      setStatus(status, "Pick a serial port…");
      await releaseCurrentPort();
      currentPort = await navigator.serial.requestPort();
      await enterRepl(/* fromFlash */ false);
    } catch (err) {
      setStatus(status, openErrorMessage(err), true);
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
    setStatus(status, "Pick a serial port…");
    await releaseCurrentPort();
    currentPort = await navigator.serial.requestPort();

    setStatus(status, "Fetching firmware…");
    const res = await fetch(`./${row.file}`);
    if (!res.ok) throw new Error(`firmware fetch ${res.status} for ${row.file}`);
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

    await loader.after("hard_reset");
    await transport.disconnect();
    transport = null;
    setStatus(status, "Flashed. Click Connect REPL to talk to the device.");
    repl.hidden = false;
  } catch (err) {
    setStatus(status, `Flash failed: ${err.message ?? err}${portLockHint(err)}`, true);
    if (transport) {
      try { await transport.disconnect(); } catch {}
    }
    await releaseCurrentPort();
    for (const el of lockables) el.disabled = false;
  }
}

// Cleanly cancel reader, release writer, await readDone, close port.
// Safe to call when nothing is open. Without this, repeat Flash attempts
// or page reloads leave Arc/Chrome holding the OS-level FD.
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
  document.getElementById("connect").addEventListener("click", () => {
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
    setStatus(status, `${openErrorMessage(err)}`, true);
    currentPort = null;
    return;
  }

  writer = currentPort.writable.getWriter();
  log.replaceChildren();
  readDone = readLoop(log);

  // Pulse the chip into a clean fresh-boot state on every REPL connect.
  // Without this:
  //   - After Connect-to-board, WebSerial's default signal state on a
  //     CP2102 board can leave EN low (chip held in reset) — no UART
  //     output, no response to typed input.
  //   - After Flash, esptool's hard_reset + transport.disconnect leaves
  //     the chip in a state where the next port.open() finds it
  //     unresponsive; an active reset pulse is required to recover.
  // Two timing quirks (observed empirically on Chrome 121 + CP2102):
  //   (1) setSignals issued immediately after port.open() is silently
  //       no-op — a settle delay is required.
  //   (2) The pulse must run *after* readLoop has called reader.read(),
  //       otherwise the chip's boot output is dropped.
  // Sequence: settle, assert RTS (EN low = reset), hold, deassert RTS
  // (run). DTR stays low throughout so IO0 is high — chip boots into
  // the app, not the bootloader. setSignals is best-effort; on boards
  // without an auto-reset circuit it's a harmless no-op.
  try {
    await new Promise((r) => setTimeout(r, 500));
    await currentPort.setSignals({ requestToSend: true, dataTerminalReady: false });
    await new Promise((r) => setTimeout(r, 200));
    await currentPort.setSignals({ requestToSend: false, dataTerminalReady: false });
  } catch {}

  picker.hidden = true;
  repl.hidden = false;
  connectBtn.hidden = true;
  log.hidden = false;
  line.hidden = false;
  disconnectBtn.hidden = false;
  setStatus(status, fromFlash
    ? "REPL connected at 115200 (post-flash). Boot output appears below."
    : "REPL connected at 115200.");
  line.focus();

  line.addEventListener("keydown", lineKeydown);
}

async function lineKeydown(e) {
  if (e.key !== "Enter" || !writer) return;
  e.preventDefault();
  const value = e.target.value;
  e.target.value = "";
  try {
    await writer.write(encoder.encode(value + "\n"));
  } catch (err) {
    appendLog(document.getElementById("log"), `(write failed: ${err.message ?? err})\n`);
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
  log.replaceChildren();
  line.value = "";
  log.hidden = true;
  line.hidden = true;
  disconnectBtn.hidden = true;
  connectBtn.hidden = false;
  repl.hidden = true;
  picker.hidden = false;
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
      const text = decoder.decode(value, { stream: true });
      if (text) appendLog(log, text);
    }
  } catch (err) {
    appendLog(log, `(read loop ended: ${err.message ?? err})\n`);
  } finally {
    try { reader.releaseLock(); } catch {}
  }
}

// Append text as a single text node and trim oldest nodes when the cap is
// hit. textContent += would re-serialize the whole log on every chunk,
// which is O(n²) and locks the page after a few hundred lines.
function appendLog(log, text) {
  log.appendChild(document.createTextNode(text));
  while (log.childNodes.length > LOG_MAX_NODES) {
    log.removeChild(log.firstChild);
  }
  log.scrollTop = log.scrollHeight;
}

function openErrorMessage(err) {
  const msg = err?.message ?? String(err);
  if (/already open|InvalidStateError|access|locked/i.test(msg)) {
    return `Open failed (${msg}). The serial port is likely held by another tab or browser process. Close all Frothy flasher tabs and try again; if that doesn't free it, restart the browser.`;
  }
  return `Open failed: ${msg}`;
}

function portLockHint(err) {
  const msg = err?.message ?? String(err);
  if (/Failed to connect|access|locked/i.test(msg)) {
    return ` — if this is a re-flash attempt, the serial port may be locked by another tab; close all Frothy flasher tabs or restart the browser to release it.`;
  }
  return "";
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
