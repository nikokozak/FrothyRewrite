import { test } from "node:test";
import assert from "node:assert/strict";
import { createConnector } from "../src/index.ts";
import { FakeTransport } from "../src/fake.ts";

// Wire v1.1: "! " is a reserved async-event prefix. A handler can fire while a
// request is in flight — including between the device reading the request's
// bytes — and print an async line into the stream. Those lines must reach line
// subscribers (so a host can show events) but never fold into the request's
// response; otherwise an event firing in the request window corrupts the value.
test("async '! ' lines reach subscribers but stay out of the response", async () => {
  const fake = new FakeTransport((line) =>
    line === "run" ? "first\n! tick\nsecond\nok\n> " : "ok\n> ",
  );
  const repl = await createConnector(fake);
  const events: string[] = [];
  repl.onLine((l) => {
    if (l.startsWith("! ")) events.push(l);
  });

  const res = await repl.sendLine("run");

  assert.deepEqual(res, { kind: "value", lines: ["first", "second"] });
  assert.deepEqual(events, ["! tick"]);

  await repl.close();
});
