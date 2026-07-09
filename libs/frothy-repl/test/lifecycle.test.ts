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

test("onClose fires once for close(), and late subscribers fire immediately", async () => {
  const fake = new FakeTransport(() => "ok\n> ");
  const repl = await createConnector(fake);
  let calls = 0;

  repl.onClose(() => {
    calls += 1;
  });

  await repl.close();
  await repl.close();
  assert.equal(calls, 1);

  let lateCalls = 0;
  repl.onClose(() => {
    lateCalls += 1;
  });
  assert.equal(lateCalls, 1);
});

test("onClose fires when the transport read ends", async () => {
  const fake = new FakeTransport();
  const repl = await createConnector(fake);
  let calls = 0;

  const closed = new Promise<void>((resolve) => {
    repl.onClose(() => {
      calls += 1;
      resolve();
    });
  });

  await fake.close();
  await closed;
  assert.equal(calls, 1);

  await repl.close();
  assert.equal(calls, 1);
});

test("interrupt writes Ctrl-C out of band while a send is pending", async () => {
  const fake = new FakeTransport();
  const repl = await createConnector(fake);

  const pending = repl.sendLine("forever");
  await Promise.resolve();
  assert.deepEqual(fake.writes, ["forever"]);

  await repl.interrupt();
  assert.deepEqual(fake.writes, ["forever", "\x03"]);

  fake.emit("ok\n> ");
  assert.deepEqual(await pending, { kind: "ok" });
  await repl.close();
});
