import assert from 'node:assert/strict';
import test from 'node:test';
import { formAt, splitForms, TextDocumentLike } from '../src/forms';

function document(text: string): TextDocumentLike {
  return { getText: () => text, lineCount: text.split('\n').length };
}

test('groups the same multiline bracket form as the CLI', () => {
  const forms = splitForms('boot is fn [\n  one\n]\nwords\n');
  assert.deepEqual(forms.map((form) => form.source), ['boot is fn [\none\n]', 'words']);
  assert.deepEqual(forms.map((form) => [form.startLine, form.endLine, form.complete]), [
    [0, 2, true],
    [3, 3, true],
  ]);
});

test('ignores comment delimiters with the same boundary rules as the CLI', () => {
  const forms = splitForms('-- header\nboot is fn [\n  -* ignored [ [\n  ] *-\n  one\n]\n');
  assert.equal(forms.length, 1);
  assert.equal(forms[0].source, 'boot is fn [\n-* ignored [ [\n] *-\none\n]');

  const embeddedDashes = splitForms('name--part\n');
  assert.equal(embeddedDashes[0].source, 'name--part');
});

test('matches every explicit CLI continuation ending', () => {
  for (const ending of [',', '->', 'else', 'fn', 'forever', 'if', 'is', 'repeat', 'set', 'to', 'with']) {
    const forms = splitForms(`first ${ending}\n  second\nthird\n`);
    assert.deepEqual(
      forms.map((form) => [form.source, form.startLine, form.endLine]),
      [[`first ${ending}\nsecond`, 0, 1], ['third', 2, 2]],
      ending,
    );
  }
});

test('tracks parentheses, brackets, braces, strings, and escapes across lines', () => {
  const text = [
    'shape is fn [',
    '  call: (one,',
    '    {two})',
    '  "a bracket ] and an escaped quote \\',
    '" still in the string"',
    ']',
    'next',
  ].join('\n');
  const forms = splitForms(text);
  assert.equal(forms.length, 2);
  assert.equal(forms[0].startLine, 0);
  assert.equal(forms[0].endLine, 5);
  assert.equal(forms[0].complete, true);
  assert.equal(forms[1].source, 'next');
});

test('returns an incomplete final form instead of pretending it is runnable', () => {
  const forms = splitForms('boot is fn [\n  one\n');
  assert.equal(forms.length, 1);
  assert.equal(forms[0].source, 'boot is fn [\none');
  assert.equal(forms[0].complete, false);
  assert.equal(forms[0].endLine, 1);
});

test('finds the containing form by cursor line', () => {
  const doc = document('one\n\nboot is fn [\n  two\n]\n');
  assert.equal(formAt(doc, 0)?.source, 'one');
  assert.equal(formAt(doc, 3)?.source, 'boot is fn [\ntwo\n]');
  assert.equal(formAt(doc, 4)?.source, 'boot is fn [\ntwo\n]');
  assert.equal(formAt(doc, -1), undefined);
  assert.equal(formAt(doc, doc.lineCount), undefined);
});

test('blank and comment-only lines choose the next form, then the previous form', () => {
  const doc = document('-- heading\n\none\n\n-- between\n\ntwo\n\n');
  assert.equal(formAt(doc, 0)?.source, 'one');
  assert.equal(formAt(doc, 1)?.source, 'one');
  assert.equal(formAt(doc, 3)?.source, 'two');
  assert.equal(formAt(doc, 4)?.source, 'two');
  assert.equal(formAt(doc, 5)?.source, 'two');
  assert.equal(formAt(doc, 7)?.source, 'two');
  assert.equal(formAt(document('-- only a comment\n'), 0), undefined);
});

test('uses VS Code-compatible offsets while accepting CRLF documents', () => {
  const text = 'boot is fn [\r\n  one\r\n]\r\nwords\r\n';
  const forms = splitForms(text);
  assert.equal(forms[0].source, 'boot is fn [\none\n]');
  assert.equal(forms[0].startOffset, 0);
  assert.equal(forms[0].endOffset, text.indexOf(']\r\n') + 1);
  assert.equal(formAt(document(text), 1)?.source, forms[0].source);
});
