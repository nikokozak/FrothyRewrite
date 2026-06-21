import { ChildProcess, spawn } from 'child_process';
import * as vscode from 'vscode';
import { appendChunk, appendLine, flushPartial, show } from './output';

let child: ChildProcess | undefined;
let onConnectionChanged: (() => void) | undefined;

export function isConnected(): boolean {
  return child !== undefined && child.exitCode === null && !child.killed;
}

export function setOnConnectionChanged(cb: () => void): void {
  onConnectionChanged = cb;
}

function fireConnectionChanged(): void {
  if (onConnectionChanged) onConnectionChanged();
}

export async function connect(): Promise<void> {
  await teardown();

  const cfg = vscode.workspace.getConfiguration('frothy');
  const bin = cfg.get<string>('binaryPath', 'frothy');
  const port = cfg.get<string>('port', '');
  const baud = cfg.get<number>('baud', 115200);

  const args = ['connect'];
  if (port) {
    args.push('--port', port);
  }
  args.push('--baud', String(baud));

  show();
  appendLine(`$ ${bin} ${args.join(' ')}`);

  let c: ChildProcess;
  try {
    c = spawn(bin, args, { stdio: ['pipe', 'pipe', 'pipe'] });
  } catch (err) {
    appendLine(`connect: spawn failed: ${(err as Error).message}`);
    return;
  }
  child = c;
  fireConnectionChanged();

  c.stdout?.on('data', (data: Buffer) => appendChunk(data.toString('utf8')));
  c.stderr?.on('data', (data: Buffer) => appendChunk(data.toString('utf8')));
  c.on('error', (err) => appendLine(`connect: ${err.message}`));
  c.on('exit', (code, signal) => {
    flushPartial();
    appendLine(`connect: exited (code=${code ?? '-'}, signal=${signal ?? '-'})`);
    if (child === c) {
      child = undefined;
      fireConnectionChanged();
    }
  });
}

export function writeLine(text: string): boolean {
  if (!isConnected() || !child?.stdin) return false;
  child.stdin.write(text + '\n');
  return true;
}

export function writeByte(b: number): boolean {
  if (!isConnected() || !child?.stdin) return false;
  child.stdin.write(Buffer.from([b]));
  return true;
}

// D14: SIGINT, wait up to 1 s, then SIGKILL. The child treats SIGINT as a
// shutdown signal (connect_unix.go:41-46); device-interrupt (byte 0x03) goes
// through writeByte instead.
export async function teardown(): Promise<void> {
  if (!child) return;
  const c = child;
  c.kill('SIGINT');
  const exited = await waitForExit(c, 1000);
  if (!exited) {
    c.kill('SIGKILL');
    await waitForExit(c, 1000);
  }
  if (child === c) {
    child = undefined;
    fireConnectionChanged();
  }
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
