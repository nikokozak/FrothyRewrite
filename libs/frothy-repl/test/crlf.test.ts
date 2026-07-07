import { test } from "node:test";
import assert from "node:assert/strict";
import { createConnector } from "../src/index.ts";
import { FakeTransport } from "../src/fake.ts";

// The ESP32 target injects a carriage return before every newline
// (targets/esp-idf/platform.c fr_platform_write_text; sdkconfig
// CONFIG_NEWLIB_STDOUT_LINE_ENDING_CRLF=y), so response lines arrive as
// "<text>\r\n". The prompt "> " carries no newline and stays bare. The
// connector must frame on \n and tolerate the trailing \r, exactly as the
// canonical Go pump does. Without this, "ok\r" fails the terminator check,
// the prompt is never accepted, and the request hangs forever — which then
// blocks the send queue and every later line.

test("CRLF line endings: ok terminator resolves", async () => {
  const fake = new FakeTransport((line) => `${line} done\r\nok\r\n> `);
  const repl = await createConnector(fake);
  assert.deepEqual(await repl.sendLine("go"), { kind: "value", lines: ["go done"] });
  await repl.close();
});

test("CRLF line endings: bare ok resolves", async () => {
  const fake = new FakeTransport(() => "ok\r\n> ");
  const repl = await createConnector(fake);
  assert.deepEqual(await repl.sendLine("noop"), { kind: "ok" });
  await repl.close();
});

test("CRLF line endings: error terminator resolves and parses", async () => {
  const fake = new FakeTransport(() => "error: bad source (8)\r\n> ");
  const repl = await createConnector(fake);
  assert.deepEqual(await repl.sendLine("1 +"), { kind: "error", code: 8, phrase: "bad source" });
  await repl.close();
});

test("CRLF line endings: subscriber sees clean lines (no trailing CR)", async () => {
  const fake = new FakeTransport(() => "7\r\nok\r\n> ");
  const repl = await createConnector(fake);
  const seen: string[] = [];
  repl.onLine((l) => seen.push(l));
  await repl.sendLine("7");
  assert.deepEqual(seen, ["7", "ok"]);
  await repl.close();
});
