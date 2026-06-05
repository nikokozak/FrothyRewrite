// Page module: detect → (picker → flash → REPL) | fallback.
// Slice A: detect + D6 fallback. Slice B: manifest picker + D8 persistence.
// Later slices wire flash + REPL.

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
}

function option(value) {
  const opt = document.createElement("option");
  opt.value = value;
  opt.textContent = value;
  return opt;
}
