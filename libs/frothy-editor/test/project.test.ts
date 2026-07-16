import { readFileSync } from "node:fs";
import { test } from "node:test";
import assert from "node:assert/strict";

import {
  acknowledgeBrowserDraft,
  createBrowserDraft,
  migrateBrowserDraft,
  validateBrowserDraft,
  validateProjectDocument,
} from "../src/project.js";
import type {
  BrowserDraft,
  Instrument,
  ProjectDocumentV1,
} from "../src/project.js";

interface FixtureCase {
  name: string;
  project_fields?: Record<string, unknown>;
  add_files?: Record<string, unknown>;
  add_repeated_file?: { path: string; character: string; bytes: number };
  add_repeated_files?: { count: number; bytes: number };
  add_plots?: number;
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

test("project: cloud acknowledgement preserves a newer local edit", () => {
  const draft = createBrowserDraft("first\n");
  const savedDocument = structuredClone(draft.document);

  const acknowledged = acknowledgeBrowserDraft(
    draft,
    "11111111-1111-1111-1111-111111111111",
    "Signals",
    1,
    savedDocument,
  );
  assert.equal(acknowledged.pendingCloudSave, false);

  const edited = structuredClone(acknowledged);
  edited.document.files["main.fr"] = "second\n";

  const staleAcknowledgement = acknowledgeBrowserDraft(
    edited,
    acknowledged.cloudProjectId!,
    acknowledged.cloudProjectTitle!,
    2,
    savedDocument,
  );
  assert.equal(staleAcknowledgement.pendingCloudSave, true);
  assert.equal(staleAcknowledgement.cloudProjectTitle, "Signals");
  assert.equal(staleAcknowledgement.document.files["main.fr"], "second\n");
});

test("project: version-0.3 drafts gain a nullable cloud title", () => {
  const { cloudProjectTitle: _removed, ...oldDraft } = createBrowserDraft("hello\n");

  const migrated = migrateBrowserDraft(oldDraft) as BrowserDraft;
  assert.equal(migrated.cloudProjectTitle, null);
  assert.deepEqual(validateBrowserDraft(migrated), []);
});

test("project: version-1 fixtures have the expected errors", () => {
  assert.equal(fixtures.fixture_schema, 1);

  for (const fixture of fixtures.cases) {
    const document = structuredClone(fixtures.base_document);
    if (fixture.project_fields) {
      Object.assign(document, fixture.project_fields);
    }
    if (fixture.add_files) {
      Object.assign(document.files, fixture.add_files);
    }
    if (fixture.add_repeated_file) {
      const addition = fixture.add_repeated_file;
      document.files[addition.path] = addition.character.repeat(addition.bytes);
    }
    if (fixture.add_repeated_files) {
      for (let index = 0; index < fixture.add_repeated_files.count; index += 1) {
        document.files[`generated/${index}.fr`] = "x".repeat(fixture.add_repeated_files.bytes);
      }
    }
    if (fixture.instruments) {
      document.instruments = fixture.instruments;
    }
    if (fixture.add_plots) {
      for (let index = 0; index < fixture.add_plots; index += 1) {
        document.instruments.push({
          id: `generated-plot-${index}`,
          kind: "plot",
          signal: `generated-signal-${index}`,
        });
      }
    }

    const beforeValidation = structuredClone(document);
    assert.deepEqual(
      validateProjectDocument(document),
      fixture.expected_errors,
      fixture.name,
    );
    assert.deepEqual(document, beforeValidation, `${fixture.name} mutated the document`);
  }
});
