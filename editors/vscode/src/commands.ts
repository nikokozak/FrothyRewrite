import { spawn } from 'child_process';
import * as vscode from 'vscode';
import * as proc from './connect';
import { appendChunk, appendLine, flushPartial, show } from './output';

// Frothy identifiers may contain `.`, `?`, `_`, `-`, and `$name` constants
// start with `$` (parse.c:235-257, 262-364).
const FROTHY_WORD = /[A-Za-z_$][A-Za-z0-9_.\-?]*/;

export function connect(): Promise<void> {
  return proc.connect();
}

// D9: split the selection on '\n', write each line followed by '\n' to the
// running child's stdin.
export function sendSelection(): void {
  const editor = vscode.window.activeTextEditor;
  if (!editor) return;
  const text = editor.document.getText(editor.selection);
  if (text.length === 0) return;
  if (!proc.isConnected()) {
    notConnectedHint();
    return;
  }
  const lines = text.split(/\r?\n/);
  while (lines.length > 0 && lines[lines.length - 1] === '') lines.pop();
  for (const line of lines) proc.writeLine(line);
}

// D1: one-shot `frothy send <path> [--port] --baud`. Save the buffer first
// so the on-disk file matches what the user sees.
export async function sendFile(): Promise<void> {
  const editor = vscode.window.activeTextEditor;
  if (!editor) return;
  const doc = editor.document;
  if (doc.isUntitled) {
    appendLine('send: save the file first.');
    show();
    return;
  }
  if (doc.isDirty) await doc.save();

  const cfg = vscode.workspace.getConfiguration('frothy');
  const bin = cfg.get<string>('binaryPath', 'frothy');
  const port = cfg.get<string>('port', '');
  const baud = cfg.get<number>('baud', 115200);

  // `frothy send` requires --port unless --dry-run is set
  // (cmd/frothy-session/main.go:2000-2002). Refuse rather than spawn a child
  // that will exit 2 with an unfamiliar error.
  if (!port) {
    appendLine('send: set frothy.port in workspace settings first.');
    show();
    return;
  }

  const args = ['send', doc.fileName, '--port', port, '--baud', String(baud)];

  show();
  appendLine(`$ ${bin} ${args.join(' ')}`);

  let c;
  try {
    c = spawn(bin, args, { stdio: ['ignore', 'pipe', 'pipe'] });
  } catch (err) {
    appendLine(`send: spawn failed: ${(err as Error).message}`);
    return;
  }
  c.stdout?.on('data', (d: Buffer) => appendChunk(d.toString('utf8')));
  c.stderr?.on('data', (d: Buffer) => appendChunk(d.toString('utf8')));
  c.on('error', (err) => appendLine(`send: ${err.message}`));
  c.on('exit', (code, signal) => {
    flushPartial();
    appendLine(`send: exited (code=${code ?? '-'}, signal=${signal ?? '-'})`);
  });
}

// D1: send `see <word>` for the symbol under cursor (or the active selection).
export function see(): void {
  const editor = vscode.window.activeTextEditor;
  if (!editor) return;
  const word = wordUnderCursor(editor);
  if (!word) return;
  if (!proc.writeLine(`see ${word}`)) notConnectedHint();
}

export function words(): void {
  if (!proc.writeLine('words')) notConnectedHint();
}

export function save(): void {
  if (!proc.writeLine('save')) notConnectedHint();
}

export function restore(): void {
  if (!proc.writeLine('restore')) notConnectedHint();
}

// D10: byte 0x03 on the child's stdin. The CLI input parser turns it into
// inputInterrupt (connect_input.go:119-121).
export function interrupt(): void {
  if (!proc.writeByte(0x03)) notConnectedHint();
}

function wordUnderCursor(editor: vscode.TextEditor): string {
  const sel = editor.selection;
  if (!sel.isEmpty) return editor.document.getText(sel);
  const range = editor.document.getWordRangeAtPosition(sel.active, FROTHY_WORD);
  return range ? editor.document.getText(range) : '';
}

function notConnectedHint(): void {
  appendLine('frothy: not connected — run "Frothy: Connect" first.');
  show();
}
