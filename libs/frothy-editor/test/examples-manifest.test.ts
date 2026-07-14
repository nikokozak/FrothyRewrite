import { test } from "node:test";
import assert from "node:assert/strict";

import { FROTHY_EXAMPLES } from "../src/examples.generated.js";
import type { FrothyExample } from "../src/examples.generated.js";

const generatorPath = "../../../tools/gen-examples-manifest.mjs";
const { parseExample } = await import(generatorPath) as {
  parseExample(name: string, text: string): FrothyExample;
};

test("examples manifest: generated entries are complete", () => {
  for (const example of FROTHY_EXAMPLES) {
    assert.ok(example.name, "name");
    assert.ok(example.title, `${example.name}: title`);
    assert.ok(example.tags.length > 0, `${example.name}: tags`);
    assert.ok(example.source.startsWith("--"), `${example.name}: source header`);
  }
});

test("examples manifest: includes the known examples in filename order", () => {
  assert.deepEqual(FROTHY_EXAMPLES.map((example) => example.name), [
    "01-hello",
    "02-words",
    "03-truthiness",
    "04-loops",
    "05-cells",
    "06-records",
    "07-attempt-rescue",
    "08-blink",
    "09-sensor",
    "10-events",
    "11-persistence",
    "12-i2c-trace",
    "13-ws2812-pulse",
  ]);
});

test("examples manifest: parser rejects examples without a header tag", () => {
  assert.throws(
    () => parseExample("x", "no header body\n"),
    /missing title line/,
  );
  assert.throws(
    () => parseExample("x", "-- Title\n-- blurb\n\nbody\n"),
    /missing -- @tag line/,
  );
});
