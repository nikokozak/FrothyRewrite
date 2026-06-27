import { ChildProcess, spawn } from 'child_process';
import * as vscode from 'vscode';
import { appendChunk, appendLine, flushPartial, show } from './output';

let child: ChildProcess | undefined;
let onConnectionChanged: (() => void) | undefined;
let intentionalExit = false;
// Re-entrancy guard. Edges this prevents:
//  - spam-clicking Connect / the status bar item spawning two children
//  - autoConnect firing concurrently for several .fr files opened at once
//  - palette-triggered connect racing a sidebar-triggered connect
// Concurrent callers all await the same in-flight promise.
let connecting: Promise<void> | undefined;

export function isConnected(): boolean {
  return child !== undefined && child.exitCode === null && !child.killed;
}

export function setOnConnectionChanged(cb: () => void): void {
  onConnectionChanged = cb;
}

function fireConnectionChanged(): void {
  if (onConnectionChanged) onConnectionChanged();
}

export function connect(): Promise<void> {
  if (connecting) return connecting;
  connecting = doConnect().finally(() => {
    connecting = undefined;
  });
  return connecting;
}

async function doConnect(): Promise<void> {
  await teardown();

  const cfg = vscode.workspace.getConfiguration('frothy');
  const bin = cfg.get<string>('binaryPath', 'frothy');
  const port = cfg.get<string>('port', '');
  const baud = cfg.get<number>('baud', 115200);

  const args = ['session'];
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
    appendLine(`session: spawn failed: ${(err as Error).message}`);
    return;
  }
  child = c;
  intentionalExit = false;
  fireConnectionChanged();

  c.stdout?.on('data', (data: Buffer) => appendChunk(data.toString('utf8')));
  c.stderr?.on('data', (data: Buffer) => appendChunk(data.toString('utf8')));
  c.on('error', (err) => {
    appendLine(`session: ${err.message}`);
    if (child === c) {
      child = undefined;
      fireConnectionChanged();
    }
  });
  c.on('exit', (code, signal) => {
    if (child !== c) return;
    flushPartial();
    if (intentionalExit) {
      appendLine('session: disconnected');
    } else {
      appendLine(`session: exited (code=${code ?? '-'}, signal=${signal ?? '-'})`);
    }
    child = undefined;
    fireConnectionChanged();
  });
}

// Catch EPIPE / "write after end" / destroyed-stream errors that fire
// when the child dies between isConnected() and write(). The caller
// gets `false` so it can surface a "not connected" warning, same as a
// genuine disconnect.
//
export function writeLine(text: string): boolean {
  if (!isConnected() || !child?.stdin || child.stdin.destroyed) return false;
  try {
    show();
    child.stdin.write(text + '\n');
    return true;
  } catch (err) {
    appendLine(`write: ${(err as Error).message}`);
    return false;
  }
}

export function writeSourceBlock(text: string, path?: string): boolean {
  if (!isConnected() || !child?.stdin || child.stdin.destroyed) return false;
  const header = path ? `.source ${path}` : '.source';
  try {
    show();
    child.stdin.write(`${header}\n${text}\n.end-source\n`);
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
  if (!child) return;
  const c = child;
  intentionalExit = true;
  c.stdin?.end();
  let exited = await waitForExit(c, 1000);
  if (exited) {
    if (child === c) {
      child = undefined;
      fireConnectionChanged();
    }
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
