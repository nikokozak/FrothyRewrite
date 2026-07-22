import assert from 'node:assert/strict';
import { chmodSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { delimiter, join } from 'node:path';
import { after, test } from 'node:test';

import { findFrothy } from '../src/cli';

const dirs: string[] = [];
function binDir(executable: boolean): string {
  const dir = mkdtempSync(join(tmpdir(), 'frothy-cli-'));
  dirs.push(dir);
  writeFileSync(join(dir, 'frothy'), '#!/bin/sh\n');
  chmodSync(join(dir, 'frothy'), executable ? 0o755 : 0o644);
  return dir;
}
after(() => dirs.forEach((dir) => rmSync(dir, { recursive: true, force: true })));

test('a configured path wins, verbatim', () => {
  assert.equal(findFrothy('/somewhere/frothy', '/unused', 'darwin', []), '/somewhere/frothy');
});

test('empty setting finds frothy on PATH', () => {
  const hit = binDir(true);
  const envPath = ['/nonexistent-dir', binDir(false), hit].join(delimiter);
  assert.equal(findFrothy('', envPath, 'darwin', []), join(hit, 'frothy'));
});

test('empty setting falls back to the Homebrew locations', () => {
  const brew = join(binDir(true), 'frothy');
  assert.equal(findFrothy('', '/nonexistent-dir', 'darwin', [brew]), brew);
});

test('nothing found keeps the bare name for the ENOENT guidance', () => {
  assert.equal(findFrothy('', '/nonexistent-dir', 'darwin', []), 'frothy');
});
