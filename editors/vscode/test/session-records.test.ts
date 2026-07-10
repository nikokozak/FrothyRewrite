import assert from 'node:assert/strict';
import test from 'node:test';
import {
  emptySessionSnapshot,
  parseSessionRecord,
  reduceSessionRecord,
  SessionSnapshot,
} from '../src/session-records';

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
      mirror: 'clean',
      mode: 'host-required',
      device: { profile: 'test' },
    }),
    record({
      v: 1,
      session: 's1',
      seq: 3,
      kind: 'send',
      state: 'waiting',
      mirror: 'pending',
      source: 'time is 200',
      line: 'apply 0102',
    }),
    record({
      v: 1,
      session: 's1',
      seq: 4,
      kind: 'response',
      state: 'idle',
      mirror: 'clean',
      status: 'ok',
      text: 'ok\n',
    }),
    record({
      v: 1,
      session: 's1',
      seq: 5,
      kind: 'compile_error',
      state: 'idle',
      mirror: 'clean',
      status: 'error: bad source (8)',
      text: 'error: bad source (8)\n',
    }),
    record({
      v: 1,
      session: 's1',
      seq: 6,
      kind: 'interrupt',
      state: 'idle',
      mirror: 'clean',
      settled: true,
      text: 'error: interrupted (10)\n',
    }),
    record({ v: 1, session: 's1', seq: 7, kind: 'session_end', state: 'closed', mirror: 'clean' }),
  ]) {
    snapshot = reduceSessionRecord(snapshot, next);
  }

  assert.equal(snapshot.state, 'closed');
  assert.equal(snapshot.mirror, 'clean');
  assert.equal(snapshot.profile, 'test');
  assert.equal(snapshot.mode, 'host-required');
  assert.equal(snapshot.lastError?.status, 'error: bad source (8)');
  assert.equal(snapshot.lastResultText, 'error: interrupted (10)\n');
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
