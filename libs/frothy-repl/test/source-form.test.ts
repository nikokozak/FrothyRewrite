import assert from "node:assert/strict";
import test from "node:test";

import { createConnector, FakeTransport } from "../src/index.ts";

test("sendForm preserves source newlines and hides its wire echo", async () => {
  const fake = new FakeTransport((line) => `${line}\r\n42\r\nok\r\n> `);
  const repl = await createConnector(fake);
  const seen: string[] = [];
  repl.onLine((line) => seen.push(line));

  const response = await repl.sendForm('to f [\r\n  print: "a\\nb"\r]');

  assert.deepEqual(fake.writes, ['source-form to f [\\n  print: "a\\\\nb"\\n]']);
  assert.deepEqual(response, { kind: "value", lines: ["42"] });
  assert.deepEqual(seen, ["42", "ok"]);
  assert.deepEqual(repl.transcript(), ["42", "ok"]);
  await repl.close();
});

test("sendLine still rejects physical newlines", async () => {
  const repl = await createConnector(new FakeTransport());
  await assert.rejects(repl.sendLine("one\ntwo"), RangeError);
  await repl.close();
});

test("sendForm keeps single-line source compatible and hides its echo", async () => {
  const fake = new FakeTransport((line) => `${line}\nok\n> `);
  const repl = await createConnector(fake);
  const seen: string[] = [];
  repl.onLine((line) => seen.push(line));

  assert.deepEqual(await repl.sendForm("7"), { kind: "ok" });
  assert.deepEqual(fake.writes, ["7"]);
  assert.deepEqual(seen, ["ok"]);
  await repl.close();
});
