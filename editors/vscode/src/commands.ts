import * as vscode from 'vscode';
import * as proc from './connect';
import { FROTHY_EXAMPLES } from './examples.generated';
import { formAt, splitForms } from './forms';
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

export async function runForm(): Promise<void> {
  const editor = vscode.window.activeTextEditor;
  if (!editor) return;
  if (!proc.isConnected()) {
    notConnectedHint();
    return;
  }

  let source: string | undefined;
  if (!editor.selection.isEmpty) {
    const forms = splitForms(editor.document.getText(editor.selection));
    if (forms.length === 1 && forms[0].complete) source = forms[0].source;
  } else {
    const form = formAt(editor.document, editor.selection.active.line);
    if (form?.complete) source = form.source;
  }
  if (!source) {
    await oneFormHint();
    return;
  }
  await submitForm(source, true);
}

export async function rerun(): Promise<void> {
  if (!proc.isConnected()) {
    notConnectedHint();
    return;
  }
  if (!lastForm) return;
  await submitForm(lastForm, false);
}

export async function runFile(): Promise<void> {
  const editor = vscode.window.activeTextEditor;
  if (!editor) return;
  if (!proc.isConnected()) {
    notConnectedHint();
    return;
  }
  const doc = editor.document;
  const text = doc.getText();
  const forms = splitForms(text);
  if (forms.length === 0) {
    vscode.window.showInformationMessage('Frothy: this file has no forms to run.');
    return;
  }
  if (forms.some((form) => !form.complete)) {
    vscode.window.showWarningMessage('Frothy: finish the incomplete form before running this file.');
    return;
  }

  const path = doc.isUntitled ? undefined : doc.fileName;
  const basename = path ? path.split(/[/\\]/).pop() ?? path : 'untitled buffer';
  const lineCount = doc.lineCount;

  show();
  appendLine(`> [run file: ${basename} · ${lineCount} lines]`);
  if (!proc.writeSourceBlock(text, path)) notConnectedHint();
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

async function submitForm(source: string, remember: boolean): Promise<void> {
  if (remember) setLastForm(source);
  try {
    await proc.request(source);
  } catch (err) {
    if (!(err instanceof proc.SessionRecordError)) {
      vscode.window.showWarningMessage(`Frothy: ${(err as Error).message}`);
    }
  }
}

async function oneFormHint(): Promise<void> {
  const choice = await vscode.window.showWarningMessage(
    'Frothy: select one complete form, or run the whole file.',
    'Run File',
  );
  if (choice === 'Run File') await vscode.commands.executeCommand('frothy.runFile');
}
