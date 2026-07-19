import assert from "node:assert/strict";
import test from "node:test";

import { parseExample } from "./gen-examples-manifest.mjs";

test("example headers require a title and tags", () => {
  assert.throws(() => parseExample("x", "no header body\n"), /missing title line/);
  assert.throws(
    () => parseExample("x", "-- Title\n-- blurb\n\nbody\n"),
    /missing -- @tag line/,
  );
});
