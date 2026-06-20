import { test } from "node:test";
import assert from "node:assert/strict";
import { createConnector, TransportError } from "../src/index.ts";
import { FakeTransport } from "../src/fake.ts";

// Invariant 6 (SPEC §"Test discipline"): a transport failure surfaces as a typed
// TransportError, never as a {kind:"error"} wire response. Prevents a port
// failure being mistaken for a Frothy protocol error and ignored.
test("transport error surface: a failed write rejects with TransportError", async () => {
  const fake = new FakeTransport();
  fake.failWrite = true;
  const repl = await createConnector(fake);

  await assert.rejects(repl.sendLine("noop"), (e) => e instanceof TransportError);

  await repl.close();
});
