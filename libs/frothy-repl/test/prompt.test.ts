import { test } from "node:test";
import assert from "node:assert/strict";
import { createConnector } from "../src/index.ts";
import { FakeTransport } from "../src/fake.ts";

// Invariant 4 (SPEC §"Test discipline"): end-of-response detection fires only on
// a real wire prompt, never on prompt-shaped bytes inside a response line.
// Prevents a truncated response being read as complete.
test("prompt detection: prompt-shaped body lines stay in the value", async () => {
  const fake = new FakeTransport((line) =>
    line === "echo" ? "a > b\n> still body\nok\n> " : "ok\n> ",
  );
  const repl = await createConnector(fake);

  const res = await repl.sendLine("echo");

  assert.deepEqual(res, { kind: "value", lines: ["a > b", "> still body"] });

  await repl.close();
});
