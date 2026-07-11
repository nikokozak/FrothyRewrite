// Invariant: a sketch saved to localStorage and reloaded comes back
// byte-for-byte. User-model corruption it prevents: a student writes a
// 50-line program, refreshes, finds it gone.

import { test } from "node:test";
import assert from "node:assert/strict";
import { JSDOM } from "jsdom";

import { DEFAULT_INITIAL_SOURCE, sendableLines, shouldConfirmReplace } from "../src/editor.js";
import { makeStorage } from "../src/storage.js";

function withDOM(): void {
  const dom = new JSDOM("<!doctype html><html><body></body></html>", {
    url: "http://localhost/",
  });
  // localStorage lives on dom.window; copy onto globalThis so the
  // storage module's globalThis.localStorage reads see it.
  Object.defineProperty(globalThis, "localStorage", {
    configurable: true,
    writable: true,
    value: dom.window.localStorage,
  });
  (globalThis as unknown as { document: Document }).document = dom.window.document;
}

test("storage: load() returns null when key is unset", () => {
  withDOM();
  const s = makeStorage("frothy-editor:test-unset");
  assert.equal(s.load(), null);
});

test("storage: save then load round-trips the source", () => {
  withDOM();
  const key = "frothy-editor:test-roundtrip";
  const s = makeStorage(key);
  const sketch = `to greet [ "hello, world" ]\ngreet\n`;
  assert.equal(s.save(sketch), true);
  assert.equal(s.load(), sketch);

  // A fresh storage handle reads the same key.
  const s2 = makeStorage(key);
  assert.equal(s2.load(), sketch);
});

test("storage: save reports missing localStorage without throwing", () => {
  // Simulate a non-browser environment by stripping localStorage.
  delete (globalThis as unknown as { localStorage?: Storage }).localStorage;
  const s = makeStorage("frothy-editor:test-no-dom");
  assert.equal(s.save("hello"), false);
  assert.equal(s.load(), null);
});

test("storage: save reports a localStorage write failure", () => {
  Object.defineProperty(globalThis, "localStorage", {
    configurable: true,
    writable: true,
    value: {
      getItem: () => null,
      setItem: () => { throw new Error("quota exceeded"); },
    },
  });
  const s = makeStorage("frothy-editor:test-throws");
  assert.equal(s.save("hello"), false);
});

test("editor sendableLines drops blanks and full-line comments only", () => {
  const source = `
-- header comment
  1 + 1 -- => 2

to greet [ "hello, world" ]
  -- another comment
greet:
`;

  assert.deepEqual(sendableLines(source), [
    "1 + 1 -- => 2",
    'to greet [ "hello, world" ]',
    "greet:",
  ]);
});

test("editor default source starts with a comment and sends a zero-arg call", () => {
  assert.deepEqual(sendableLines(DEFAULT_INITIAL_SOURCE), [
    'to greet [ "hello, world" ]',
    "greet:",
  ]);
});

test("editor sendableLines returns no lines for blank/comment-only source", () => {
  assert.deepEqual(sendableLines(" \n-- nothing here\n\t-- still a comment\n"), []);
});

test("editor shouldConfirmReplace protects real non-default sketches only", () => {
  assert.equal(shouldConfirmReplace("", "to greet [ 1 ]"), false);
  assert.equal(shouldConfirmReplace(DEFAULT_INITIAL_SOURCE, "to greet [ 1 ]"), false);
  assert.equal(shouldConfirmReplace("to greet [ 1 ]\n", "to greet [ 1 ]"), false);
  assert.equal(shouldConfirmReplace("to greet [ 2 ]", "to greet [ 1 ]"), true);
});
