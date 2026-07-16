import { readFileSync } from "node:fs";
import { test } from "node:test";
import assert from "node:assert/strict";

import {
  createBrowserDraft,
  validateBrowserDraft,
  validateProjectDocument,
} from "../src/project.js";
import type {
  Instrument,
  ProjectDocumentV1,
} from "../src/project.js";

interface FixtureCase {
  name: string;
  add_file?: { path: string; source: string };
  add_repeated_file?: { path: string; character: string; bytes: number };
  instruments?: Instrument[];
  expected_errors: string[];
}

interface FixtureSet {
  fixture_schema: number;
  base_document: ProjectDocumentV1;
  cases: FixtureCase[];
}

const fixtures = JSON.parse(
  readFileSync(
    new URL("../fixtures/project-document-v1.json", import.meta.url),
    "utf8",
  ),
) as FixtureSet;

test("project: legacy source becomes main.fr byte-for-byte", () => {
  const source = `to greet [ "hello" ]\ngreet:\n`;
  const draft = createBrowserDraft(source);

  assert.equal(draft.document.files["main.fr"], source);
  assert.match(draft.document.files["frothy.toml"] ?? "", /board = "esp32_devkit_v1"/);
  assert.deepEqual(validateBrowserDraft(draft), []);
});

test("project: version-1 fixtures have the expected errors", () => {
  assert.equal(fixtures.fixture_schema, 1);

  for (const fixture of fixtures.cases) {
    const document = structuredClone(fixtures.base_document);
    if (fixture.add_file) {
      document.files[fixture.add_file.path] = fixture.add_file.source;
    }
    if (fixture.add_repeated_file) {
      const addition = fixture.add_repeated_file;
      document.files[addition.path] = addition.character.repeat(addition.bytes);
    }
    if (fixture.instruments) {
      document.instruments = fixture.instruments;
    }

    assert.deepEqual(
      validateProjectDocument(document),
      fixture.expected_errors,
      fixture.name,
    );
  }
});
