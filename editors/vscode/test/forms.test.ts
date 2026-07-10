import assert from 'node:assert/strict';
import test from 'node:test';
import { sourceFormAt, sourceForms } from '../src/forms';

test('groups the same multiline bracket form as the CLI', () => {
  const forms = sourceForms('boot is fn [\n  one\n]\nwords\n');
  assert.deepEqual(forms.map((form) => form.source), ['boot is fn [ one ]', 'words']);
  assert.deepEqual(forms.map((form) => [form.startLine, form.endLine, form.complete]), [
    [0, 2, true],
    [3, 3, true],
  ]);
});

test('ignores comment delimiters with the same boundary rules as the CLI', () => {
  const forms = sourceForms('-- header\nboot is fn [\n  -* ignored [ [\n  ] *-\n  one\n]\n');
  assert.equal(forms.length, 1);
  assert.equal(forms[0].source, 'boot is fn [ -* ignored [ [ ] *- one ]');

  const embeddedDashes = sourceForms('name--part\n');
  assert.equal(embeddedDashes[0].source, 'name--part');
});

test('matches every explicit CLI continuation ending', () => {
  for (const ending of [',', '->', 'else', 'fn', 'forever', 'if', 'is', 'repeat', 'set', 'to', 'with']) {
    const forms = sourceForms(`first ${ending}\n  second\nthird\n`);
    assert.deepEqual(
      forms.map((form) => [form.source, form.startLine, form.endLine]),
      [[`first ${ending} second`, 0, 1], ['third', 2, 2]],
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
  const forms = sourceForms(text);
  assert.equal(forms.length, 2);
  assert.equal(forms[0].startLine, 0);
  assert.equal(forms[0].endLine, 5);
  assert.equal(forms[0].complete, true);
  assert.equal(forms[1].source, 'next');
});

test('returns an incomplete final form instead of pretending it is runnable', () => {
  const forms = sourceForms('boot is fn [\n  one\n');
  assert.equal(forms.length, 1);
  assert.equal(forms[0].source, 'boot is fn [ one');
  assert.equal(forms[0].complete, false);
  assert.equal(forms[0].endLine, 1);
});

test('finds a form only when the cursor is within its document range', () => {
  const text = 'one\n\nboot is fn [\n  two\n]\n';
  assert.equal(sourceFormAt(text, text.indexOf('one'))?.source, 'one');
  assert.equal(sourceFormAt(text, text.indexOf('two'))?.source, 'boot is fn [ two ]');
  assert.equal(sourceFormAt(text, text.indexOf('\n') + 1), undefined);
  assert.equal(sourceFormAt(text, text.indexOf(']') + 1)?.source, 'boot is fn [ two ]');
});

test('uses VS Code-compatible offsets while accepting CRLF documents', () => {
  const text = 'boot is fn [\r\n  one\r\n]\r\nwords\r\n';
  const forms = sourceForms(text);
  assert.equal(forms[0].source, 'boot is fn [ one ]');
  assert.equal(forms[0].startOffset, 0);
  assert.equal(forms[0].endOffset, text.indexOf(']\r\n') + 1);
  assert.equal(sourceFormAt(text, text.indexOf('one'))?.source, forms[0].source);
});
