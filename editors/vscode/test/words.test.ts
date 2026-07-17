import assert from 'node:assert/strict';
import { test } from 'node:test';
import { parseWords } from '../src/words';

test('parseWords strips the echo line and trailing ok', () => {
  assert.deepEqual(parseWords('words\nblink led-on led-off\nok'), ['blink', 'led-on', 'led-off']);
});

test('parseWords handles wrapped lines, CRLF, and duplicates', () => {
  assert.deepEqual(parseWords('words\r\nblink wait\r\nblink loop\r\nok'), ['blink', 'wait', 'loop']);
});

test('parseWords returns empty for an empty vocabulary', () => {
  assert.deepEqual(parseWords('words\nok'), []);
  assert.deepEqual(parseWords(''), []);
});
