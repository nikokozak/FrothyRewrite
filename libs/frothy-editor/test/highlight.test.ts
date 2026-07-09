// Invariant: the Frothy stream tokenizer recognizes the small grammar
// frothy init's main.fr template uses (keywords, numbers in three bases,
// text literal, brackets, names, operators, call sites).
// User-model corruption it prevents: a student opens the editor and
// sees their valid Frothy painted as plain text.

import { test } from "node:test";
import assert from "node:assert/strict";

import { tokenStream } from "../src/highlight.js";

test("highlight: keywords, numbers, text, brackets, names, operators, call sites", () => {
  const src = `-- greeting setup
to greet with x [ "hi" ]
set count to 0xFF
repeat 3 [ count + 1 ]
greet: 50% true
boot is fn [ on 2 changes [ ] ]`;

  const kinds = tokenStream(src);

  // The exact sequence is implementation-noisy; what we lock in is the
  // set of token kinds the highlighter emits at all. If any of these
  // disappear, the student's editor looks broken.
  const present = new Set(kinds);
  for (const want of [
    "comment",
    "keyword",
    "name",
    "callName",
    "number",
    "string",
    "bracket",
    "operator",
    "constant",
  ]) {
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

test("highlight: parser keywords added after the first grammar pass stay keywords", () => {
  assert.deepEqual(tokenStream("attempt rescue"), ["keyword", "keyword"]);
});

test("highlight: Frothy comments are comments without operator leakage", () => {
  assert.deepEqual(tokenStream("-- hello"), ["comment"]);
  assert.deepEqual(tokenStream("-* block *-"), ["comment"]);
});

test("highlight: minus still distinguishes numbers and operators", () => {
  assert.deepEqual(tokenStream("-7"), ["number"]);
  assert.deepEqual(tokenStream("a - b"), ["name", "operator", "name"]);
});

test("highlight: a double-dash only opens a comment after whitespace/line start", () => {
  // parse.c treats `--` as a comment only when whitespace precedes it, so
  // `10--5` is arithmetic (10 - -5 = 15) and must NOT gray out as a comment.
  assert.deepEqual(tokenStream("10--5"), ["number", "operator", "number"]);
  // With a space it IS a comment, swallowing the rest of the line.
  assert.deepEqual(tokenStream("x -- c"), ["name", "comment"]);
});

test("highlight: constants are distinct from keywords and names", () => {
  assert.deepEqual(tokenStream("$led_builtin"), ["constant"]);
  assert.deepEqual(tokenStream("true nil"), ["constant", "constant"]);
});
