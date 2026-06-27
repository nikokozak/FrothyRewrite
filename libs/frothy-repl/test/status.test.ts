import { test } from "node:test";
import assert from "node:assert/strict";
import { createConnector, WireFormatError } from "../src/index.ts";
import { FakeTransport } from "../src/fake.ts";

// Invariant 1 (SPEC §"Test discipline"): the status-line parser tracks the
// wire-protocol field order and meaning. Prevents a host believing it talks to
// one profile when it talks to another.
const STATUS_LINE =
  "frothy status v1 profile=esp32_plain profile_hash=a1b2c3d4 compiler=device names=device storage=eeprom interrupt=cooperative word_size=32 int_min=-1073741824 int_max=1073741823 apply_bytes=2048";

test("status round-trip: parsed fields equal the line, malformed fields reject", async () => {
  const good = new FakeTransport((line) =>
    line === "status" ? `${STATUS_LINE}\nok\n> ` : "ok\n> ",
  );
  const repl = await createConnector(good);

  assert.deepEqual(await repl.status(), {
    version: 1,
    profile: "esp32_plain",
    profile_hash: "a1b2c3d4",
    compiler: "device",
    names: "device",
    storage: "eeprom",
    interrupt: "cooperative",
    word_size: 32,
    int_min: -1073741824,
    int_max: 1073741823,
    apply_bytes: 2048,
  });

  await repl.close();

  // A bad enum or a non-integer numeric must reject as WireFormatError, never
  // leak through as a typed status — the host must not misread the profile.
  for (const bad of [
    STATUS_LINE.replace("storage=eeprom", "storage=flash"),
    STATUS_LINE.replace("apply_bytes=2048", "apply_bytes=lots"),
  ]) {
    const fake = new FakeTransport((line) =>
      line === "status" ? `${bad}\nok\n> ` : "ok\n> ",
    );
    const conn = await createConnector(fake);

    await assert.rejects(conn.status(), (e) => e instanceof WireFormatError);

    await conn.close();
  }
});

test("status retry: framing-shaped first response retries once", async () => {
  let attempts = 0;
  const fake = new FakeTransport((line) => {
    if (line !== "status") return "ok\n> ";
    attempts += 1;
    return attempts === 1 ? "frothy status v1 profilestatus\nok\n> " : `${STATUS_LINE}\nok\n> `;
  });
  const conn = await createConnector(fake);

  const status = await conn.status();

  assert.equal(status.profile, "esp32_plain");
  assert.equal(attempts, 2);
  await conn.close();
});

test("status retry: truncated first status line retries once", async () => {
  let attempts = 0;
  const fake = new FakeTransport((line) => {
    if (line !== "status") return "ok\n> ";
    attempts += 1;
    return attempts === 1 ? "frothy status v1\nok\n> " : `${STATUS_LINE}\nok\n> `;
  });
  const conn = await createConnector(fake);

  const status = await conn.status();

  assert.equal(status.profile, "esp32_plain");
  assert.equal(attempts, 2);
  await conn.close();
});

test("status retry: semantic bad status does not retry", async () => {
  let attempts = 0;
  const fake = new FakeTransport((line) => {
    if (line !== "status") return "ok\n> ";
    attempts += 1;
    return `${STATUS_LINE.replace("storage=eeprom", "storage=flash")}\nok\n> `;
  });
  const conn = await createConnector(fake);

  await assert.rejects(conn.status(), (e) => e instanceof WireFormatError);

  assert.equal(attempts, 1);
  await conn.close();
});

test("status retry: device error does not retry", async () => {
  let attempts = 0;
  const fake = new FakeTransport((line) => {
    if (line !== "status") return "ok\n> ";
    attempts += 1;
    return "error: unsupported (9)\n> ";
  });
  const conn = await createConnector(fake);

  await assert.rejects(conn.status(), /status failed: unsupported \(9\)/);

  assert.equal(attempts, 1);
  await conn.close();
});
