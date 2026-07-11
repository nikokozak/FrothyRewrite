import * as vscode from 'vscode';
import { SessionRecord } from './session-records';

let collection: vscode.DiagnosticCollection | undefined;

export function initDiagnostics(context: vscode.ExtensionContext): void {
  collection = vscode.languages.createDiagnosticCollection('frothy');
  context.subscriptions.push(collection);
}

export function clearDiagnostics(document: vscode.TextDocument): void {
  collection?.delete(document.uri);
}

export function reportFormError(
  document: vscode.TextDocument,
  range: vscode.Range,
  submittedVersion: number,
  record: SessionRecord,
): void {
  if (!collection || document.version !== submittedVersion || !documentIsOpen(document)) return;
  const diagnostic = new vscode.Diagnostic(range, recordMessage(record), vscode.DiagnosticSeverity.Error);
  diagnostic.source = 'Frothy';
  collection.set(document.uri, [diagnostic]);
}

function recordMessage(record: SessionRecord): string {
  const status = stringField(record, 'status');
  const text = stringField(record, 'text')?.trim();
  if (!status) return text || 'Frothy rejected this form.';
  if (!text || text === status || text.startsWith(status)) return text || status;
  return `${status}\n${text}`;
}

function stringField(record: SessionRecord, key: string): string | undefined {
  return typeof record[key] === 'string' ? record[key] : undefined;
}

function documentIsOpen(document: vscode.TextDocument): boolean {
  return vscode.workspace.textDocuments.some((open) => open.uri.toString() === document.uri.toString());
}
