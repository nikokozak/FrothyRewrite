import { test } from "node:test";
import assert from "node:assert/strict";

import { FROTHY_EXAMPLES } from "../src/examples.generated.js";
import { formAt, splitForms } from "../src/forms.js";

test("forms: groups the CLI multiline fixture into one sendable line", () => {
  const forms = splitForms("boot is fn [\n  one\n]\nwords\n");
  assert.deepEqual(forms.map((form) => form.source), ["boot is fn [ one ]", "words"]);
  assert.deepEqual(forms.map((form) => [form.startLine, form.endLine, form.complete]), [
    [0, 2, true],
    [3, 3, true],
  ]);
});

test("forms: removes comments without treating string or name dashes as comments", () => {
  const forms = splitForms([
    "-- heading [",
    "boot is fn [",
    "  -* ignored [ [",
    "  ] *-",
    "  print: \"-- still text ]\" -- trailing ]",
    "  name--part",
    "]",
  ].join("\n"));
  assert.equal(forms.length, 1);
  assert.equal(forms[0].source, 'boot is fn [ print: "-- still text ]" name--part ]');
});

test("forms: matches every explicit CLI continuation ending", () => {
  for (const ending of [",", "->", "else", "fn", "forever", "if", "is", "repeat", "set", "to", "with"]) {
    const forms = splitForms(`first ${ending}\n  second\nthird\n`);
    assert.deepEqual(forms.map((form) => form.source), [`first ${ending} second`, "third"], ending);
  }
});

test("forms: tracks all delimiters, strings, escapes, and incomplete input", () => {
  const complete = splitForms([
    "shape is fn [",
    "  call: (one,",
    "    {two})",
    "  \"a bracket ] and an escaped quote \\",
    "\" still in the string\"",
    "]",
  ].join("\n"));
  assert.equal(complete.length, 1);
  assert.equal(complete[0].complete, true);

  const incomplete = splitForms("boot is fn [\n  one\n");
  assert.equal(incomplete.length, 1);
  assert.equal(incomplete[0].complete, false);
});

test("forms: blank and comment lines choose next, then previous", () => {
  const text = "-- heading\n\none\n\n-- between\n\ntwo\n\n";
  assert.equal(formAt(text, 0)?.source, "one");
  assert.equal(formAt(text, 3)?.source, "two");
  assert.equal(formAt(text, 7)?.source, "two");
  assert.equal(formAt("-- only a comment\n", 0), undefined);
});

test("forms: every bundled example contains only complete forms", () => {
  for (const example of FROTHY_EXAMPLES) {
    const forms = splitForms(example.source);
    assert.ok(forms.length > 0, example.name);
    assert.ok(forms.every((form) => form.complete), example.name);
  }
});
