import { test } from "node:test";
import assert from "node:assert/strict";

import {
  createBrowserDraft,
  PROJECT_LIMITS,
  validateBrowserDraft,
  validateProjectDocument,
} from "../src/project.js";

test("project: legacy source becomes main.fr byte-for-byte", () => {
  const source = `to greet [ "hello" ]\ngreet:\n`;
  const draft = createBrowserDraft(source);

  assert.equal(draft.document.files["main.fr"], source);
  assert.match(draft.document.files["frothy.toml"] ?? "", /board = "esp32_devkit_v1"/);
  assert.deepEqual(validateBrowserDraft(draft), []);
});

test("project: rejects unsafe paths and oversized source", () => {
  const draft = createBrowserDraft("hello\n");
  draft.document.files["../secret.fr"] = "nope";
  draft.document.files["large.fr"] = "x".repeat(PROJECT_LIMITS.fileBytes + 1);

  const errors = validateProjectDocument(draft.document);
  assert(errors.some((error) => error.includes("normalized relative path")));
  assert(errors.some((error) => error.includes("large.fr exceeds")));
});

test("project: validates control and plot instruments", () => {
  const draft = createBrowserDraft("hello\n");
  draft.document.instruments = [
    {
      id: "speed-control",
      kind: "control",
      binding: "speed",
      value_type: "int",
      min: 0,
      max: 255,
      step: 1,
    },
    {
      id: "temperature-plot",
      kind: "plot",
      signal: "temperature",
      unit: "C",
      scale: 0.01,
    },
  ];

  assert.deepEqual(validateProjectDocument(draft.document), []);
  draft.document.instruments[1] = { ...draft.document.instruments[1], id: "speed-control" };
  assert(
    validateProjectDocument(draft.document)
      .some((error) => error.includes("repeats speed-control")),
  );
});
