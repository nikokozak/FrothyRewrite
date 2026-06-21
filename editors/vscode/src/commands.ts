import { spawn } from 'child_process';
import * as vscode from 'vscode';
import * as proc from './connect';
import { appendChunk, appendLine, flushPartial, show } from './output';

// Frothy identifiers may contain `.`, `?`, `_`, `-`, and `$name` constants
// start with `$` (parse.c:235-257, 262-364).
const FROTHY_WORD = /[A-Za-z_$][A-Za-z0-9_.\-?]*/;

let lastForm: string | undefined;
let onLastFormChanged: (() => void) | undefined;

export function hasLastForm(): boolean {
  return lastForm !== undefined && lastForm.length > 0;
}

export function setOnLastFormChanged(cb: () => void): void {
  onLastFormChanged = cb;
}

function setLastForm(form: string | undefined): void {
  lastForm = form;
  if (onLastFormChanged) onLastFormChanged();
}

export function connect(): Promise<void> {
  return proc.connect();
}

export async function disconnect(): Promise<void> {
  await proc.teardown();
}

// Send a single line — the current cursor line if no selection, otherwise
// the first line of the selection. Updates lastForm.
export function runLine(): void {
  const editor = vscode.window.activeTextEditor;
  if (!editor) return;
  if (!proc.isConnected()) {
    notConnectedHint();
    return;
  }
  const line = currentLineText(editor);
  if (!line) return;
  setLastForm(line);
  proc.writeLine(line);
}

// Send every non-empty line in the selection, in order. Updates lastForm
// to the last line sent so runLast can repeat.
export function sendSelection(): void {
  const editor = vscode.window.activeTextEditor;
  if (!editor) return;
  if (!proc.isConnected()) {
    notConnectedHint();
    return;
  }
  const text = editor.selection.isEmpty
    ? currentLineText(editor)
    : editor.document.getText(editor.selection);
  if (!text) return;
  const lines = text.split(/\r?\n/).map((l) => l.trim()).filter((l) => l.length > 0);
  if (lines.length === 0) return;
  for (const line of lines) proc.writeLine(line);
  setLastForm(lines[lines.length - 1]);
}

// Repeat whatever runLine / sendSelection last sent.
export function runLast(): void {
  if (!proc.isConnected()) {
    notConnectedHint();
    return;
  }
  if (!lastForm) return;
  proc.writeLine(lastForm);
}

// One-shot `frothy send <path> [--port] --baud`. Save the buffer first so
// the on-disk file matches what the user sees.
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

// `see <word>` for the symbol under the cursor (or the active selection).
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

export function status(): void {
  if (!proc.writeLine('status')) notConnectedHint();
}

export function mem(): void {
  if (!proc.writeLine('mem')) notConnectedHint();
}

export function save(): void {
  if (!proc.writeLine('save')) notConnectedHint();
}

export function restore(): void {
  if (!proc.writeLine('restore')) notConnectedHint();
}

// Byte 0x03 on the child's stdin. The CLI input parser turns it into
// inputInterrupt (connect_input.go:119-121).
export function interrupt(): void {
  if (!proc.writeByte(0x03)) notConnectedHint();
}

function currentLineText(editor: vscode.TextEditor): string {
  const head = editor.selection.active;
  return editor.document.lineAt(head.line).text.trim();
}

function wordUnderCursor(editor: vscode.TextEditor): string {
  const sel = editor.selection;
  if (!sel.isEmpty) return editor.document.getText(sel);
  const range = editor.document.getWordRangeAtPosition(sel.active, FROTHY_WORD);
  return range ? editor.document.getText(range) : '';
}

function notConnectedHint(): void {
  vscode.window.showWarningMessage(
    'Frothy: not connected. Click the status bar item or run "Frothy: Connect".',
  );
}
