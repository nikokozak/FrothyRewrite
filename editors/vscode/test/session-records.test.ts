import assert from 'node:assert/strict';
import test from 'node:test';
import {
  emptySessionSnapshot,
  parseSessionRecord,
  recordFailed,
  reduceSessionRecord,
  SessionSnapshot,
} from '../src/session-records';
import transcriptGrammar from '../syntaxes/frothy-transcript.tmLanguage.json';

function record(fields: Record<string, unknown>) {
  return parseSessionRecord(JSON.stringify(fields));
}

test('the record parser accepts only the v1 envelope while allowing future kinds', () => {
  const future = record({
    v: 1,
    session: 's1',
    seq: 1,
    kind: 'future_observation',
    state: 'idle',
    mirror: 'clean',
    future_field: 42,
  });
  assert.equal(future.kind, 'future_observation');
  assert.equal(future.future_field, 42);

  const invalid = [
    'not JSON',
    'null',
    '[]',
    JSON.stringify({ v: 2, session: 's1', seq: 1, kind: 'status', state: 'idle', mirror: 'clean' }),
    JSON.stringify({ v: 1, session: '', seq: 1, kind: 'status', state: 'idle', mirror: 'clean' }),
    JSON.stringify({ v: 1, session: 's1', seq: 0, kind: 'status', state: 'idle', mirror: 'clean' }),
    JSON.stringify({ v: 1, session: 's1', seq: 1.5, kind: 'status', state: 'idle', mirror: 'clean' }),
    JSON.stringify({ v: 1, session: 's1', seq: 1, kind: '', state: 'idle', mirror: 'clean' }),
    JSON.stringify({ v: 1, session: 's1', seq: 1, kind: 'status', state: 'strange', mirror: 'clean' }),
    JSON.stringify({ v: 1, session: 's1', seq: 1, kind: 'status', state: 'idle', mirror: 'strange' }),
  ];
  for (const line of invalid) assert.throws(() => parseSessionRecord(line));
});

test('the reducer derives the small live editor snapshot from session records', () => {
  let snapshot: SessionSnapshot = emptySessionSnapshot();
  for (const next of [
    record({ v: 1, session: 's1', seq: 1, kind: 'session_start', state: 'syncing', mirror: 'none' }),
    record({
      v: 1,
      session: 's1',
      seq: 2,
      kind: 'status',
      state: 'idle',
      mirror: 'none',
      mode: 'device',
      device: { profile: 'test' },
    }),
    record({
      v: 1,
      session: 's1',
      seq: 3,
      kind: 'send',
      state: 'waiting',
      mirror: 'none',
      source: 'time is 200',
      line: 'time is 200',
    }),
    record({
      v: 1,
      session: 's1',
      seq: 4,
      kind: 'response',
      state: 'idle',
      mirror: 'none',
      status: 'ok',
      ok: true,
      text: 'ok\n',
    }),
    record({
      v: 1,
      session: 's1',
      seq: 5,
      kind: 'compile_error',
      state: 'idle',
      mirror: 'none',
      status: 'error: bad source (8)',
      text: 'error: bad source (8)\n',
    }),
    record({
      v: 1,
      session: 's1',
      seq: 6,
      kind: 'interrupt',
      state: 'idle',
      mirror: 'none',
      settled: true,
      text: 'error: interrupted (10)\n',
    }),
    record({ v: 1, session: 's1', seq: 7, kind: 'session_end', state: 'closed', mirror: 'none' }),
  ]) {
    snapshot = reduceSessionRecord(snapshot, next);
  }

  assert.equal(snapshot.state, 'closed');
  assert.equal(snapshot.mirror, 'none');
  assert.equal(snapshot.profile, 'test');
  assert.equal(snapshot.mode, 'device');
  assert.equal(snapshot.lastError?.status, 'error: bad source (8)');
  assert.equal(snapshot.lastResultText, 'error: interrupted (10)\n');
});

test('failed device responses and legacy compile errors reject editor requests', () => {
  assert.equal(recordFailed(record({
    v: 1, session: 's1', seq: 1, kind: 'response', state: 'idle', mirror: 'none', ok: false,
  })), true);
  assert.equal(recordFailed(record({
    v: 1, session: 's1', seq: 2, kind: 'response', state: 'idle', mirror: 'none', ok: true,
  })), false);
  assert.equal(recordFailed(record({
    v: 1, session: 's1', seq: 3, kind: 'compile_error', state: 'idle', mirror: 'clean',
  })), true);
});

test('device notices remain successful results and warning-colored output', () => {
  const noticeText = 'notice: not saved (13)\ndetail: still live\nok\n';
  const notice = record({
    v: 1,
    session: 's1',
    seq: 2,
    kind: 'response',
    state: 'idle',
    mirror: 'none',
    status: 'ok',
    ok: true,
    notice: 'notice: not saved (13)',
    text: noticeText,
  });

  const before = reduceSessionRecord(emptySessionSnapshot(), record({
    v: 1,
    session: 's1',
    seq: 1,
    kind: 'compile_error',
    state: 'idle',
    mirror: 'none',
    status: 'error: bad source (8)',
  }));
  const after = reduceSessionRecord(before, notice);

  assert.equal(recordFailed(notice), false);
  assert.equal(notice.notice, 'notice: not saved (13)');
  assert.equal(after.lastError, undefined);
  assert.equal(after.lastResultText, noticeText);

  assert.ok(transcriptGrammar.patterns.some((pattern) => pattern.include === '#notice-line'));
  const noticeRule = transcriptGrammar.repository['notice-line'];
  assert.equal(noticeRule.name, 'token.warn-token.frothy-transcript');
  // Keep this canonical shape aligned with cmd/frothy-session/main.go.
  assert.match('notice: not saved (13)', new RegExp(noticeRule.match));
  assert.doesNotMatch('notice: malformed', new RegExp(noticeRule.match));
});

test('stale state stays visible through unknown records and the terminal error', () => {
  let snapshot = emptySessionSnapshot();
  for (const next of [
    record({ v: 1, session: 's1', seq: 1, kind: 'session_start', state: 'syncing', mirror: 'none' }),
    record({ v: 1, session: 's1', seq: 2, kind: 'status', state: 'idle', mirror: 'clean' }),
    record({ v: 1, session: 's1', seq: 3, kind: 'send', state: 'waiting', mirror: 'pending' }),
    record({
      v: 1,
      session: 's1',
      seq: 4,
      kind: 'interrupt',
      state: 'stale',
      mirror: 'stale',
      settled: true,
      text: 'error: interrupted (10)\n',
    }),
    record({ v: 1, session: 's1', seq: 5, kind: 'future_observation', state: 'stale', mirror: 'stale' }),
    record({
      v: 1,
      session: 's1',
      seq: 6,
      kind: 'session_error',
      state: 'stale',
      mirror: 'stale',
      code: 'mirror_stale',
      message: 'device update interrupted; compiler mirror stale',
    }),
  ]) {
    snapshot = reduceSessionRecord(snapshot, next);
  }

  assert.equal(snapshot.state, 'stale');
  assert.equal(snapshot.mirror, 'stale');
  assert.equal(snapshot.lastError?.kind, 'session_error');
  assert.equal(snapshot.lastError?.code, 'mirror_stale');
  assert.equal(snapshot.lastResultText, 'error: interrupted (10)\n');
});
