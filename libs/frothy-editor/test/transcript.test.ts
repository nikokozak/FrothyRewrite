import { test } from "node:test";
import assert from "node:assert/strict";
import { JSDOM } from "jsdom";

import { mountTranscript } from "../src/transcript.js";

function classNames(root: Element): string[] {
  return Array.from(root.children, (child) => child.className);
}

test("transcript groups post-error diagnostic lines until ok", () => {
  const dom = new JSDOM("<!doctype html><div id=\"host\"></div>");
  const host = dom.window.document.querySelector("#host")!;
  const transcript = mountTranscript(host as HTMLElement);

  transcript.appendDevice("error: not found (7)");
  transcript.appendDevice("name: x");
  transcript.appendDevice("^^^");
  transcript.appendDevice("help: define x first");
  transcript.appendDevice("ok");

  assert.deepEqual(classNames(transcript.element), [
    "transcript-error",
    "transcript-detail",
    "transcript-detail",
    "transcript-detail",
    "transcript-ok",
  ]);
});

test("transcript host lines reset diagnostic grouping", () => {
  const dom = new JSDOM("<!doctype html><div id=\"host\"></div>");
  const host = dom.window.document.querySelector("#host")!;
  const transcript = mountTranscript(host as HTMLElement);

  transcript.appendDevice("error: not found (7)");
  transcript.appendDevice("name: x");
  transcript.appendHost("1 2 +");
  transcript.appendDevice("name: later");

  assert.deepEqual(classNames(transcript.element), [
    "transcript-error",
    "transcript-detail",
    "transcript-host",
    "transcript-line",
  ]);
});

test("transcript: an async event after an error is not grouped into it", () => {
  const dom = new JSDOM("<!doctype html><div id=\"host\"></div>");
  const host = dom.window.document.querySelector("#host")!;
  const transcript = mountTranscript(host as HTMLElement);

  // The diagnostic block ends at a bare prompt (no line); an event firing
  // before the user types again must not latch onto the stale error.
  transcript.appendDevice("error: not found (7)");
  transcript.appendDevice("name: x");
  transcript.appendDevice("! tick");
  transcript.appendDevice("! tick");

  assert.deepEqual(classNames(transcript.element), [
    "transcript-error",
    "transcript-detail",
    "transcript-line",
    "transcript-line",
  ]);
});

test("transcript: a system note ends a diagnostic group and renders plainly", () => {
  const dom = new JSDOM("<!doctype html><div id=\"host\"></div>");
  const host = dom.window.document.querySelector("#host")!;
  const transcript = mountTranscript(host as HTMLElement);

  transcript.appendDevice("error: not found (7)");
  transcript.appendDevice("name: x");
  transcript.note("stopped: line errored");
  transcript.appendDevice("name: later");

  assert.deepEqual(classNames(transcript.element), [
    "transcript-error",
    "transcript-detail",
    "transcript-note",
    "transcript-line",
  ]);
});

test("transcript is a bounded live log that can be cleared", () => {
  const dom = new JSDOM("<!doctype html><div id=\"host\"></div>");
  const host = dom.window.document.querySelector("#host")!;
  const transcript = mountTranscript(host as HTMLElement);

  for (let index = 0; index < 1_000; index += 1) {
    transcript.appendDevice(`line-${index}`);
  }

  assert.equal(transcript.element.getAttribute("role"), "log");
  assert.equal(transcript.element.getAttribute("aria-live"), "polite");
  assert.equal(transcript.element.children.length, 800);
  assert.equal(transcript.element.firstElementChild?.textContent, "line-200");
  assert.equal(transcript.element.lastElementChild?.textContent, "line-999");

  transcript.clear();
  assert.equal(transcript.element.children.length, 0);
});
