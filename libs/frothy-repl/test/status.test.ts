import { test } from "node:test";
import assert from "node:assert/strict";
import { createConnector, WireFormatError } from "../src/index.ts";
import { FakeTransport } from "../src/fake.ts";

// Invariant 1 (SPEC §"Test discipline"): the status-line parser tracks the
// wire-protocol field order and meaning. Prevents a host believing it talks to
// one profile when it talks to another.
const STATUS_LINE =
  "frothy status v1 profile=esp32_plain profile_hash=a1b2c3d4 compiler=device names=device storage=eeprom interrupt=cooperative word_size=32 int_min=-1073741824 int_max=1073741823 apply_bytes=2048";

test("status round-trip: parsed fields equal the wire-protocol status line", async () => {
  const fake = new FakeTransport((line) =>
    line === "status" ? `${STATUS_LINE}\nok\n> ` : "ok\n> ",
  );
  const repl = await createConnector(fake);

  const status = await repl.status();

  assert.deepEqual(status, {
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
});

test("status round-trip: a malformed enum or numeric field rejects, not leaks", async () => {
  for (const bad of [
    STATUS_LINE.replace("storage=eeprom", "storage=flash"),
    STATUS_LINE.replace("apply_bytes=2048", "apply_bytes=lots"),
  ]) {
    const fake = new FakeTransport((line) =>
      line === "status" ? `${bad}\nok\n> ` : "ok\n> ",
    );
    const repl = await createConnector(fake);

    await assert.rejects(repl.status(), (e) => e instanceof WireFormatError);

    await repl.close();
  }
});
