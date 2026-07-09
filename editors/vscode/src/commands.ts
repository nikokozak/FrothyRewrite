import * as vscode from 'vscode';
import * as proc from './connect';
import { FROTHY_EXAMPLES } from './examples.generated';
import { appendLine, show } from './output';

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
  if (!proc.writeLine(line)) {
    notConnectedHint();
    return;
  }
  setLastForm(line);
}

// Send every non-empty line in the selection, in order. Updates lastForm
// to the last line sent so runLast can repeat. If the connection drops
// part-way through, stop and surface the warning rather than dropping
// the rest of the lines silently.
//
// For multi-line batches, drop a "[run selection: N lines]" header into
// the transcript so the device/session echoes that follow have context — at
// 30+ lines the bare replies blur together otherwise.
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
  if (lines.length > 1) {
    appendLine(`> [run selection: ${lines.length} lines]`);
  }
  let sent: string | undefined;
  for (const line of lines) {
    if (!proc.writeLine(line)) {
      notConnectedHint();
      break;
    }
    sent = line;
  }
  if (sent) setLastForm(sent);
}

// Repeat whatever runLine / sendSelection last sent.
export function runLast(): void {
  if (!proc.isConnected()) {
    notConnectedHint();
    return;
  }
  if (!lastForm) return;
  if (!proc.writeLine(lastForm)) notConnectedHint();
}

// Send the current buffer through the open `frothy session` process. The
// optional path is only for include resolution; the source itself is the
// unsaved editor text.
export async function sendFile(): Promise<void> {
  const editor = vscode.window.activeTextEditor;
  if (!editor) return;
  if (!proc.isConnected()) {
    notConnectedHint();
    return;
  }
  const doc = editor.document;

  const path = doc.isUntitled ? undefined : doc.fileName;
  const basename = path ? path.split(/[/\\]/).pop() ?? path : 'untitled buffer';
  const lineCount = doc.lineCount;

  show();
  appendLine(`> [send file: ${basename} · ${lineCount} lines]`);
  if (!proc.writeSourceBlock(doc.getText(), path)) notConnectedHint();
}

export async function openExample(): Promise<void> {
  const pick = await vscode.window.showQuickPick(
    FROTHY_EXAMPLES.map((example) => ({
      label: example.title,
      description: example.name,
      detail: example.tags.join(' '),
      example,
    })),
  );
  if (!pick) return;
  const doc = await vscode.workspace.openTextDocument({
    language: 'frothy',
    content: pick.example.source,
  });
  await vscode.window.showTextDocument(doc);
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

// SIGINT to the `frothy session` child. The session turns that into a device
// interrupt while a foreground command is running.
export function interrupt(): void {
  if (!proc.interrupt()) notConnectedHint();
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
