import { test } from "node:test";
import assert from "node:assert/strict";
import { createConnector } from "../src/index.ts";
import { FakeTransport } from "../src/fake.ts";

// Invariant 5 (SPEC §"Test discipline"): lifecycle survives stupid sequences.
// Double close is a no-op (transport.close fires once, no double-release) and a
// fresh connector works after close. Prevents leaked ports that lock the device.
test("lifecycle idempotence: double close releases once; reopen works", async () => {
  const fake = new FakeTransport(() => "ok\n> ");
  const repl = await createConnector(fake);

  await repl.close();
  await repl.close();
  assert.equal(fake.closeCount, 1);

  const reopened = new FakeTransport(() => "ok\n> ");
  const repl2 = await createConnector(reopened);
  assert.deepEqual(await repl2.sendLine("noop"), { kind: "ok" });
  await repl2.close();
});
