import * as vscode from 'vscode';
import * as proc from './connect';
import { clearDiagnostics, reportCompileError } from './diagnostics';
import { FROTHY_EXAMPLES } from './examples.generated';
import { formAt, SourceForm, splitForms } from './forms';
import { appendLine, show } from './output';

// Frothy identifiers may contain `.`, `?`, `_`, `-`, and `$name` constants
// start with `$` (parse.c:235-257, 262-364).
const FROTHY_WORD = /[A-Za-z_$][A-Za-z0-9_.\-?]*/;

let lastForm: string | undefined;
let onLastFormChanged: (() => void) | undefined;
let connecting: Promise<void> | undefined;

interface SubmittedForm {
  source: string;
  document: vscode.TextDocument;
  range: vscode.Range;
  version: number;
}

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
  if (connecting) return connecting;
  connecting = connectWithPortChoice().finally(() => {
    connecting = undefined;
  });
  return connecting;
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

  let submitted: SubmittedForm | undefined;
  if (!editor.selection.isEmpty) {
    const text = editor.document.getText(editor.selection);
    const forms = splitForms(text);
    if (forms.length === 1 && forms[0].complete) {
      const selectionOffset = editor.document.offsetAt(editor.selection.start);
      submitted = submittedForm(editor.document, forms[0], selectionOffset);
    }
  } else {
    const form = formAt(editor.document, editor.selection.active.line);
    if (form?.complete) submitted = submittedForm(editor.document, form, 0);
  }
  if (!submitted) {
    await oneFormHint();
    return;
  }
  clearDiagnostics(editor.document);
  await submitForm(submitted.source, true, submitted);
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

  clearDiagnostics(doc);
  show();
  appendLine(`> [run file: ${basename} · ${lineCount} lines]`);
  try {
    await proc.requestSourceBlock(text, path);
  } catch (err) {
    if (!(err instanceof proc.SessionRecordError)) {
      vscode.window.showWarningMessage(`Frothy: ${(err as Error).message}`);
    }
  }
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

export async function browseWords(): Promise<void> {
  if (!proc.isConnected()) {
    notConnectedHint();
    return;
  }
  const response = await deviceRequest('words');
  if (!response) return;

  const lines = response.trim().split(/\r?\n/);
  if (lines[0]?.trim() === 'words') lines.shift();
  const words = lines.join(' ').split(/\s+/);
  if (words[words.length - 1] === 'ok') words.pop();
  const liveWords = [...new Set(words.filter(Boolean))];
  if (liveWords.length === 0) {
    vscode.window.showInformationMessage('Frothy: this device reported no inspectable words.');
    return;
  }
  const word = await vscode.window.showQuickPick(liveWords, {
    placeHolder: 'Choose a live Frothy word to inspect',
  });
  if (word) await inspectNamedWord(word);
}

export async function inspectWord(): Promise<void> {
  const editor = vscode.window.activeTextEditor;
  if (!editor) return;
  const word = wordUnderCursor(editor);
  if (!word) {
    vscode.window.showWarningMessage('Frothy: select a word or place the cursor on one.');
    return;
  }
  await inspectNamedWord(word);
}

export async function status(): Promise<void> {
  await deviceRequest('status');
}

export async function mem(): Promise<void> {
  await deviceRequest('mem');
}

export async function save(): Promise<void> {
  await deviceRequest('save');
}

export async function restore(): Promise<void> {
  await deviceRequest('restore');
}

// SIGINT to the `frothy session` child. The session turns that into a device
// interrupt while a foreground command is running.
export function interrupt(): void {
  if (!proc.interrupt()) notConnectedHint();
}

function wordUnderCursor(editor: vscode.TextEditor): string {
  const sel = editor.selection;
  if (!sel.isEmpty) {
    const selected = editor.document.getText(sel).trim();
    return new RegExp(`^${FROTHY_WORD.source}$`).test(selected) ? selected : '';
  }
  const range = editor.document.getWordRangeAtPosition(sel.active, FROTHY_WORD);
  return range ? editor.document.getText(range) : '';
}

function notConnectedHint(): void {
  vscode.window.showWarningMessage(
    'Frothy: not connected. Click the status bar item or run "Frothy: Connect".',
  );
}

async function submitForm(
  source: string,
  remember: boolean,
  submitted?: SubmittedForm,
): Promise<void> {
  if (remember) setLastForm(source);
  try {
    await proc.request(source);
  } catch (err) {
    if (err instanceof proc.SessionRecordError && err.record.kind === 'compile_error' && submitted) {
      reportCompileError(submitted.document, submitted.range, submitted.version, err.record);
    } else {
      vscode.window.showWarningMessage(`Frothy: ${(err as Error).message}`);
    }
  }
}

function submittedForm(
  document: vscode.TextDocument,
  form: SourceForm,
  baseOffset: number,
): SubmittedForm {
  return {
    source: form.source,
    document,
    range: new vscode.Range(
      document.positionAt(baseOffset + form.startOffset),
      document.positionAt(baseOffset + form.endOffset),
    ),
    version: document.version,
  };
}

async function oneFormHint(): Promise<void> {
  const choice = await vscode.window.showWarningMessage(
    'Frothy: select one complete form, or run the whole file.',
    'Run File',
  );
  if (choice === 'Run File') await vscode.commands.executeCommand('frothy.runFile');
}

async function inspectNamedWord(word: string): Promise<void> {
  if (!proc.isConnected()) {
    notConnectedHint();
    return;
  }
  await deviceRequest(`see ${word}`);
}

async function connectWithPortChoice(): Promise<void> {
  try {
    await proc.connect();
  } catch (err) {
    if (!(err instanceof proc.SessionRecordError)) throw err;
    const code = typeof err.record.code === 'string' ? err.record.code : '';
    if (code === 'no_ports') {
      const choice = await vscode.window.showErrorMessage(
        'Frothy found no serial device. Plug one in or configure frothy.port.',
        'Configure Port',
      );
      if (choice === 'Configure Port') {
        await vscode.commands.executeCommand('workbench.action.openSettings', 'frothy.port');
      }
      return;
    }
    if (code !== 'multiple_ports') throw err;

    const candidates = Array.isArray(err.record.candidates)
      ? [...new Set(err.record.candidates.filter((value): value is string =>
          typeof value === 'string' && value.length > 0))]
      : [];
    if (candidates.length === 0) throw err;
    const port = await vscode.window.showQuickPick(candidates, {
      placeHolder: 'Choose the serial port for this Frothy session',
    });
    if (port) await proc.connect(port);
  }
}

async function deviceRequest(source: string): Promise<string | undefined> {
  try {
    const response = await proc.request(source);
    const error = terminalError(response);
    if (error) {
      vscode.window.showErrorMessage(`Frothy: ${error}`);
      return undefined;
    }
    return response;
  } catch (err) {
    vscode.window.showErrorMessage(`Frothy: ${(err as Error).message}`);
    return undefined;
  }
}

function terminalError(response: string): string | undefined {
  const lines = response.trim().split(/\r?\n/);
  const terminal = lines[lines.length - 1];
  return terminal?.startsWith('error:') ? terminal : undefined;
}
