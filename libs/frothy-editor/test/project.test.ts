import { readFileSync } from "node:fs";
import { test } from "node:test";
import assert from "node:assert/strict";

import {
  addProjectFile,
  acknowledgeBrowserDraft,
  createBrowserDraft,
  DEFAULT_PROJECT_MANIFEST,
  deleteProjectFile,
  migrateBrowserDraft,
  newestBrowserDraft,
  openCloudProjectDraft,
  renameProjectFile,
  sameProjectDocument,
  selectProjectFile,
  startNewProjectDraft,
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

test("project: cloud open installs a validated independent document", () => {
  const draft = createBrowserDraft("local\n");
  const cloudDocument = createBrowserDraft("cloud\n").document;

  const opened = openCloudProjectDraft(
    draft,
    "11111111-1111-1111-1111-111111111111",
    "Cloud signals",
    3,
    cloudDocument,
  );

  cloudDocument.files["main.fr"] = "changed afterward\n";
  assert.equal(opened.cloudProjectId, "11111111-1111-1111-1111-111111111111");
  assert.equal(opened.cloudProjectTitle, "Cloud signals");
  assert.equal(opened.baseLockVersion, 3);
  assert.equal(opened.document.files["main.fr"], "cloud\n");
  assert.equal(opened.pendingCloudSave, false);
});

test("project: new project keeps source and drops the cloud association", () => {
  const cloudDraft = openCloudProjectDraft(
    createBrowserDraft("local\n"),
    "11111111-1111-1111-1111-111111111111",
    "Signals",
    2,
    createBrowserDraft("cloud\n").document,
  );

  const started = startNewProjectDraft(cloudDraft);

  assert.equal(started.document.files["main.fr"], "cloud\n");
  assert.equal(started.cloudProjectId, null);
  assert.equal(started.cloudProjectTitle, null);
  assert.equal(started.baseLockVersion, null);
  assert.equal(started.pendingCloudSave, false);
});

test("project: document comparison ignores JSON object key order", () => {
  const left = createBrowserDraft("sample\n").document;
  left.instruments = [{
    id: "signal",
    kind: "plot",
    signal: "adc",
    unit: "V",
    scale: 0.001,
  }];

  const right = {
    instruments: [{
      scale: 0.001,
      unit: "V",
      signal: "adc",
      kind: "plot",
      id: "signal",
    }],
    files: {
      "main.fr": "sample\n",
      "frothy.toml": DEFAULT_PROJECT_MANIFEST,
    },
    schema: 1,
  } as ProjectDocumentV1;

  assert.equal(sameProjectDocument(left, right), true);
  right.files["main.fr"] = "different\n";
  assert.equal(sameProjectDocument(left, right), false);
});

test("project: version-0.3 drafts gain multi-file browser state", () => {
  const {
    cloudProjectTitle: _cloudTitle,
    activePath: _activePath,
    localRevision: _localRevision,
    ...oldDraft
  } = createBrowserDraft("hello\n");

  const migrated = migrateBrowserDraft(oldDraft) as BrowserDraft;
  assert.equal(migrated.cloudProjectTitle, null);
  assert.equal(migrated.activePath, "main.fr");
  assert.equal(migrated.localRevision, 0);
  assert.deepEqual(validateBrowserDraft(migrated), []);
});

test("project: file actions preserve the whole document and required files", () => {
  const original = createBrowserDraft("main\n");
  const created = addProjectFile(original, "src/blink.fr");
  created.document.files["src/blink.fr"] = "blink\n";

  assert.equal(created.activePath, "src/blink.fr");
  assert.equal(created.document.files["main.fr"], "main\n");

  const renamed = renameProjectFile(created, "src/blink.fr", "src/led.fr");
  const selected = selectProjectFile(renamed, "frothy.toml");
  const deleted = deleteProjectFile(selected, "src/led.fr");

  assert.equal(renamed.document.files["src/led.fr"], "blink\n");
  assert.equal(renamed.document.files["src/blink.fr"], undefined);
  assert.equal(selected.activePath, "frothy.toml");
  assert.equal(deleted.document.files["main.fr"], "main\n");
  assert.equal(deleted.document.files["frothy.toml"], DEFAULT_PROJECT_MANIFEST);
  assert.equal(deleted.document.files["src/led.fr"], undefined);
  assert.ok(deleted.localRevision > original.localRevision);
  assert.equal(newestBrowserDraft(original, deleted), deleted);
  assert.equal(newestBrowserDraft(deleted, original), deleted);
  assert.throws(() => deleteProjectFile(deleted, "main.fr"), /cannot be deleted/);
  assert.throws(() => renameProjectFile(deleted, "frothy.toml", "project.fr"), /cannot be renamed/);
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
