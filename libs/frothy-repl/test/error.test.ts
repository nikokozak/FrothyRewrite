import { test } from "node:test";
import assert from "node:assert/strict";
import { createConnector } from "../src/index.ts";
import { FakeTransport } from "../src/fake.ts";

// Invariant 2 (SPEC §"Test discipline"): terminator classification is sound.
// Both the canonical and legacy error forms resolve as {kind:"error"}, never as
// value lines. Prevents the most dangerous wire bug: an error read as a value.
test("error vs value: canonical and legacy errors resolve as error, not value", async () => {
  const fake = new FakeTransport((line) =>
    line === "canonical"
      ? "error: bad source (8)\n> "
      : line === "legacy"
        ? "err 8\n> "
        : "ok\n> ",
  );
  const repl = await createConnector(fake);

  assert.deepEqual(await repl.sendLine("canonical"), {
    kind: "error",
    code: 8,
    phrase: "bad source",
  });
  assert.deepEqual(await repl.sendLine("legacy"), {
    kind: "error",
    code: 8,
    phrase: null,
  });

  await repl.close();
});
