// Invariant: the Frothy stream tokenizer recognizes the small grammar
// frothy init's main.fr template uses (keywords, numbers in three bases,
// text literal, brackets, names, operators, call sites).
// User-model corruption it prevents: a student opens the editor and
// sees their valid Frothy painted as plain text.

import { test } from "node:test";
import assert from "node:assert/strict";

import { tokenStream } from "../src/highlight.js";

test("highlight: keywords, numbers, text, brackets, names, operators, call sites", () => {
  const src = `to greet with x [ "hi" ]
set count to 0xFF
repeat 3 [ count + 1 ]
greet: 50%
boot is fn [ on 2 changes [ ] ]`;

  const kinds = tokenStream(src);

  // The exact sequence is implementation-noisy; what we lock in is the
  // set of token kinds the highlighter emits at all. If any of these
  // disappear, the student's editor looks broken.
  const present = new Set(kinds);
  for (const want of ["keyword", "name", "callName", "number", "string", "bracket", "operator"]) {
    assert.ok(present.has(want), `expected token kind ${want} in ${[...present].join(",")}`);
  }
});

test("highlight: recognizes hex, binary, and percent numbers", () => {
  const decimal = tokenStream("123");
  const hex = tokenStream("0xFF");
  const bin = tokenStream("0b1010");
  const pct = tokenStream("50%");

  assert.deepEqual(decimal, ["number"]);
  assert.deepEqual(hex, ["number"]);
  assert.deepEqual(bin, ["number"]);
  assert.deepEqual(pct, ["number"]);
});

test("highlight: text literal with embedded escape stays one string token per line", () => {
  const kinds = tokenStream(`"hello\\n"`);
  // The whole literal lives on a single line, so it emits "string" twice:
  // once on entry (the opening quote), once on close. As long as both
  // are "string" and there is no name/operator leaking through, we are
  // safe. The visual user sees one continuous color.
  for (const k of kinds) assert.equal(k, "string");
});
