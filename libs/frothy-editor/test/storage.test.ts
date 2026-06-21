// Invariant: a sketch saved to localStorage and reloaded comes back
// byte-for-byte. User-model corruption it prevents: a student writes a
// 50-line program, refreshes, finds it gone.

import { test } from "node:test";
import assert from "node:assert/strict";
import { JSDOM } from "jsdom";

import { makeStorage } from "../src/storage.js";

function withDOM(): void {
  const dom = new JSDOM("<!doctype html><html><body></body></html>", {
    url: "http://localhost/",
  });
  // localStorage lives on dom.window; copy onto globalThis so the
  // storage module's globalThis.localStorage reads see it.
  (globalThis as unknown as { localStorage: Storage }).localStorage = dom.window.localStorage;
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
  s.save(sketch);
  assert.equal(s.load(), sketch);

  // A fresh storage handle reads the same key.
  const s2 = makeStorage(key);
  assert.equal(s2.load(), sketch);
});

test("storage: save tolerates a missing localStorage (no DOM)", () => {
  // Simulate a non-browser environment by stripping localStorage.
  delete (globalThis as unknown as { localStorage?: Storage }).localStorage;
  const s = makeStorage("frothy-editor:test-no-dom");
  // Should not throw.
  s.save("hello");
  assert.equal(s.load(), null);
});
