import { ChildProcess, spawn } from 'child_process';
import * as vscode from 'vscode';
import { appendChunk, appendLine, appendSent, appendText, flushPartial, show } from './output';
import {
  emptySessionSnapshot,
  parseSessionRecord,
  reduceSessionRecord,
  SessionRecord,
  SessionSnapshot,
} from './session-records';

interface PendingRequest {
  resolve: (text: string) => void;
  reject: (error: Error) => void;
}

interface PendingSourceBlock {
  resolve: () => void;
  reject: (error: Error) => void;
}

let child: ChildProcess | undefined;
let snapshot = emptySessionSnapshot();
let editorReady = false;
let pendingRequest: PendingRequest | undefined;
let pendingSourceBlock: PendingSourceBlock | undefined;
let onConnectionChanged: (() => void) | undefined;
let intentionalExit = false;
// Re-entrancy guard. Edges this prevents:
//  - spam-clicking Connect / the status bar item spawning two children
//  - autoConnect firing concurrently for several .fr files opened at once
//  - palette-triggered connect racing a sidebar-triggered connect
// Concurrent callers all await the same in-flight promise.
let connecting: Promise<void> | undefined;

export function isConnected(): boolean {
  return editorReady && child !== undefined && child.exitCode === null && !child.killed &&
    snapshot.state !== 'stale' && snapshot.state !== 'error' && snapshot.state !== 'closed';
}

export function sessionSnapshot(): Readonly<SessionSnapshot> {
  return snapshot;
}

export function isBusy(): boolean {
  return pendingRequest !== undefined || pendingSourceBlock !== undefined ||
    snapshot.state === 'waiting' || snapshot.state === 'interrupting';
}

export function setOnConnectionChanged(cb: () => void): void {
  onConnectionChanged = cb;
}

function fireConnectionChanged(): void {
  if (onConnectionChanged) onConnectionChanged();
}

export function connect(portOverride?: string): Promise<void> {
  if (connecting) return connecting;
  connecting = doConnect(portOverride).finally(() => {
    connecting = undefined;
  });
  return connecting;
}

async function doConnect(portOverride?: string): Promise<void> {
  await teardown();

  const cfg = vscode.workspace.getConfiguration('frothy');
  const bin = cfg.get<string>('binaryPath', 'frothy');
  const port = portOverride ?? cfg.get<string>('port', '');
  const baud = cfg.get<number>('baud', 115200);

  const args = ['session', '--records'];
  if (port) {
    args.push('--port', port);
  }
  args.push('--baud', String(baud), '--settle', '0s');

  show();
  appendLine(`$ ${bin} ${args.join(' ')}`);

  let c: ChildProcess;
  try {
    c = spawn(bin, args, { stdio: ['pipe', 'pipe', 'pipe'] });
  } catch (err) {
    appendLine(`session: cannot start ${bin}: ${(err as Error).message}`);
    throw err;
  }
  child = c;
  snapshot = { state: 'syncing', mirror: 'none' };
  editorReady = false;
  intentionalExit = false;
  fireConnectionChanged();

  await new Promise<void>((resolve, reject) => {
    let stdoutBuffer = '';
    let readySettled = false;
    let recordFailed = false;

    const rejectReady = (error: Error) => {
      if (readySettled) return;
      readySettled = true;
      reject(error);
    };
    const failRecordStream = (error: Error) => {
      if (recordFailed) return;
      recordFailed = true;
      appendLine(`records: ${error.message}`);
      editorReady = false;
      snapshot = { ...snapshot, state: 'error' };
      rejectReady(error);
      rejectPending(error);
      fireConnectionChanged();
      c.kill();
    };

    c.stdout?.on('data', (data: Buffer) => {
      if (recordFailed || child !== c) return;
      stdoutBuffer += data.toString('utf8');
      for (;;) {
        const newline = stdoutBuffer.indexOf('\n');
        if (newline < 0) break;
        const line = stdoutBuffer.slice(0, newline);
        stdoutBuffer = stdoutBuffer.slice(newline + 1);
        if (line.trim().length === 0) continue;

        let record: SessionRecord;
        try {
          record = parseSessionRecord(line);
        } catch (err) {
          failRecordStream(err as Error);
          return;
        }

        if (record.kind === 'status') {
          const next = reduceSessionRecord(snapshot, record);
          if (record.state !== 'idle' || !next.profile || !next.mode) {
            failRecordStream(new Error('status record is missing editor-ready device state'));
            return;
          }
          snapshot = next;
        } else {
          snapshot = reduceSessionRecord(snapshot, record);
        }
        renderRecord(record);
        settleRequest(record);

        if (record.kind === 'status') {
          editorReady = true;
          if (!readySettled) {
            readySettled = true;
            resolve();
          }
        } else if (record.kind === 'session_error') {
          editorReady = false;
          rejectReady(new SessionRecordError(record));
        } else if (record.kind === 'session_end') {
          editorReady = false;
          rejectReady(new Error('session ended before device status'));
        }
        fireConnectionChanged();
      }
    });
    c.stderr?.on('data', (data: Buffer) => appendChunk(data.toString('utf8')));
    c.on('error', (err) => {
      appendLine(`session: cannot start ${bin}: ${err.message}`);
      editorReady = false;
      snapshot = { ...snapshot, state: 'error' };
      rejectReady(err);
      rejectPending(err);
      if (child === c) child = undefined;
      fireConnectionChanged();
    });
    c.on('exit', (code, signal) => {
      if (child !== c) return;
      flushPartial();
      const detail = `code=${code ?? '-'}, signal=${signal ?? '-'}`;
      const error = new Error(`session exited before device status (${detail})`);
      if (intentionalExit) {
        appendLine('session: disconnected');
        snapshot = { ...snapshot, state: 'closed' };
      } else {
        appendLine(`session: exited (${detail})`);
        if (snapshot.state !== 'stale' && snapshot.state !== 'error' && snapshot.state !== 'closed') {
          snapshot = { ...snapshot, state: 'error' };
        }
      }
      editorReady = false;
      rejectReady(error);
      rejectPending(error);
      child = undefined;
      fireConnectionChanged();
    });
  });
}

export class SessionRecordError extends Error {
  constructor(readonly record: SessionRecord) {
    super(recordString(record, 'message') ?? recordString(record, 'text') ??
      recordString(record, 'status') ?? `session ${record.kind}`);
    this.name = 'SessionRecordError';
  }
}

export function request(source: string): Promise<string> {
  if (!isConnected()) return Promise.reject(new Error('Frothy is not connected'));
  if (isBusy()) return Promise.reject(new Error('another Frothy request is still running'));

  return new Promise<string>((resolve, reject) => {
    pendingRequest = { resolve, reject };
    fireConnectionChanged();
    const input = source.endsWith('\n') ? source : source + '\n';
    if (!writeInput(input)) {
      pendingRequest = undefined;
      fireConnectionChanged();
      reject(new Error('Frothy session closed before the request was sent'));
    }
  });
}

function renderRecord(record: SessionRecord): void {
  switch (record.kind) {
    case 'status':
      appendLine(`session: connected (${snapshot.profile}, ${snapshot.mode})`);
      break;
    case 'send':
    case 'compile_error': {
      const source = recordString(record, 'source');
      if (source) appendSent(source);
      const text = recordString(record, 'text') ?? recordString(record, 'status');
      if (text) appendText(text);
      break;
    }
    case 'response': {
      const text = recordString(record, 'text') ?? recordString(record, 'status');
      if (text) appendText(text);
      break;
    }
    case 'interrupt': {
      const text = recordString(record, 'text') ?? recordString(record, 'status');
      if (text) appendText(text);
      break;
    }
    case 'source_end':
      appendLine('file: complete');
      break;
    case 'session_error':
      appendLine(`session: ${recordString(record, 'code') ?? 'error'}: ${recordString(record, 'message') ?? 'session failed'}`);
      break;
  }
}

function settleRequest(record: SessionRecord): void {
  if (pendingSourceBlock) {
    switch (record.kind) {
      case 'source_end': {
        const pending = pendingSourceBlock;
        pendingSourceBlock = undefined;
        pending.resolve();
        break;
      }
      case 'session_error':
        rejectPending(new SessionRecordError(record));
        break;
      case 'session_end':
        rejectPending(new Error('Frothy session ended before the file completed'));
        break;
    }
    return;
  }
  if (!pendingRequest) return;
  switch (record.kind) {
    case 'response': {
      const pending = pendingRequest;
      pendingRequest = undefined;
      pending.resolve(recordString(record, 'text') ?? '');
      break;
    }
    case 'compile_error':
    case 'interrupt':
    case 'session_error':
      rejectPending(new SessionRecordError(record));
      break;
    case 'session_end':
      rejectPending(new Error('Frothy session ended before a response'));
      break;
  }
}

function rejectPending(error: Error): void {
  const request = pendingRequest;
  const sourceBlock = pendingSourceBlock;
  pendingRequest = undefined;
  pendingSourceBlock = undefined;
  request?.reject(error);
  sourceBlock?.reject(error);
}

function recordString(record: SessionRecord, key: string): string | undefined {
  return typeof record[key] === 'string' ? record[key] : undefined;
}

export function requestSourceBlock(text: string, path?: string): Promise<void> {
  if (!isConnected()) return Promise.reject(new Error('Frothy is not connected'));
  if (isBusy()) return Promise.reject(new Error('another Frothy request is still running'));

  return new Promise<void>((resolve, reject) => {
    pendingSourceBlock = { resolve, reject };
    fireConnectionChanged();
    const header = path ? `.source ${path}` : '.source';
    if (!writeInput(`${header}\n${text}\n.end-source\n`)) {
      pendingSourceBlock = undefined;
      fireConnectionChanged();
      reject(new Error('Frothy session closed before the file was sent'));
    }
  });
}

function writeInput(text: string): boolean {
  if (!isConnected() || !child?.stdin || child.stdin.destroyed) return false;
  try {
    show();
    child.stdin.write(text);
    return true;
  } catch (err) {
    appendLine(`write: ${(err as Error).message}`);
    return false;
  }
}

export function interrupt(): boolean {
  if (!isConnected() || !child) return false;
  try {
    return child.kill('SIGINT');
  } catch (err) {
    appendLine(`interrupt: ${(err as Error).message}`);
    return false;
  }
}

// Close stdin first: an idle `frothy session` exits on EOF. If it is stuck in a
// foreground device command, SIGINT asks the session to send a device interrupt;
// SIGKILL is only the last resort.
export async function teardown(): Promise<void> {
  rejectPending(new Error('Frothy session disconnected'));
  if (!child) {
    editorReady = false;
    snapshot = emptySessionSnapshot();
    fireConnectionChanged();
    return;
  }
  const c = child;
  intentionalExit = true;
  c.stdin?.end();
  let exited = await waitForExit(c, 1000);
  if (exited) {
    if (child === c) child = undefined;
    editorReady = false;
    snapshot = emptySessionSnapshot();
    fireConnectionChanged();
    return;
  }
  c.kill('SIGINT');
  exited = await waitForExit(c, 1000);
  if (!exited) {
    c.kill('SIGKILL');
    await waitForExit(c, 1000);
  }
  if (child === c) {
    child = undefined;
  }
  editorReady = false;
  snapshot = emptySessionSnapshot();
  fireConnectionChanged();
}

function waitForExit(c: ChildProcess, ms: number): Promise<boolean> {
  return new Promise((resolve) => {
    if (c.exitCode !== null || c.signalCode !== null) {
      resolve(true);
      return;
    }
    const timer = setTimeout(() => resolve(false), ms);
    c.once('exit', () => {
      clearTimeout(timer);
      resolve(true);
    });
  });
}
