import { test } from "node:test";
import assert from "node:assert/strict";
import { createConnector } from "../src/index.ts";
import { FakeTransport } from "../src/fake.ts";

// Invariant 3 (SPEC §"Test discipline"): apply/run honor the wire protocol's
// inline-binary convention exactly. Decoding the captured hex must reproduce the
// original payload. Prevents silently corrupted overlays.
test("hex round-trip: apply writes the payload as lowercase hex, byte-exact", async () => {
  const fake = new FakeTransport(() => "ok\n> ");
  const repl = await createConnector(fake);
  const bytes = new Uint8Array([0x00, 0x0f, 0xa5, 0xff, 0x10]);

  await repl.apply(bytes);

  const hex = fake.writes[0]!.replace(/^apply /, "");
  const decoded = Uint8Array.from(
    hex.match(/../g)!.map((pair) => parseInt(pair, 16)),
  );
  assert.deepEqual(decoded, bytes);

  await repl.close();
});
